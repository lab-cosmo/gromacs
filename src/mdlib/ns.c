/*
 * $Id$
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * GROwing Monsters And Cloning Shrimps
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef GMX_THREADS
#include <pthread.h> 
#endif

#include <math.h>
#include <string.h>
#include "sysstuff.h"
#include "smalloc.h"
#include "macros.h"
#include "maths.h"
#include "vec.h"
#include "network.h"
#include "nsgrid.h"
#include "force.h"
#include "nonbonded.h"
#include "ns.h"
#include "pbc.h"
#include "names.h"
#include "fatal.h"
#include "nrnb.h"
#include "txtdump.h"


#define MAX_CG 1024

typedef struct {
  int     ncg;
  int     nj;
  atom_id jcg[MAX_CG];
} t_ns_buf;

/* 
 *    E X C L U S I O N   H A N D L I N G
 */
typedef unsigned long t_excl;

#ifdef DEBUG
static void SETEXCL_(t_excl e[],atom_id i,atom_id j)
{   e[j] = e[j] | (1<<i); }
static void RMEXCL_(t_excl e[],atom_id i,atom_id j) 
{ e[j]=e[j] & ~(1<<i); }
static bool ISEXCL_(t_excl e[],atom_id i,atom_id j) 
{ return (bool)(e[j] & (1<<i)); }
static bool NOTEXCL_(t_excl e[],atom_id i,atom_id j)
{  return !(ISEXCL(e,i,j)); }
#else
#define SETEXCL(e,i,j) (e)[((atom_id) (j))] |= (1<<((atom_id) (i)))
#define RMEXCL(e,i,j)  (e)[((atom_id) (j))] &= (~(1<<((atom_id) (i))))
#define ISEXCL(e,i,j)  (bool) ((e)[((atom_id) (j))] & (1<<((atom_id) (i))))
#define NOTEXCL(e,i,j) !(ISEXCL(e,i,j))
#endif

/************************************************
 *
 *  U T I L I T I E S    F O R    N S
 *
 ************************************************/

static int NLI_INC = 1000;
static int NLJ_INC = 16384;

static void reallocate_nblist(t_nblist *nl)
{
  if (debug)
    fprintf(debug,"reallocating neigborlist il_code=%d, maxnri=%d\n",
	    nl->il_code,nl->maxnri); 
  srenew(nl->iinr,   nl->maxnri+2);
  srenew(nl->gid,    nl->maxnri+2);
  srenew(nl->shift,  nl->maxnri+2);
  srenew(nl->jindex, nl->maxnri+2);
}

/* ivdw/icoul are used to determine the type of interaction, so we
 * can set an innerloop index here. The obvious choice for this would have
 * been the vdwtype/coultype values in the forcerecord, but unfortunately 
 * those types are braindead - for instance both Buckingham and normal 
 * Lennard-Jones use the same value (evdwCUT), and a separate boolean variable
 * to determine which interaction is used. There is further no special value
 * for 'no interaction'. For backward compatibility with old TPR files we won't
 * change this in the 3.x series, so when calling this routine you should use:
 *
 * icoul=0 no coulomb interaction
 * icoul=1 cutoff standard coulomb
 * icoul=2 reaction-field coulomb
 * icoul=3 tabulated coulomb
 *
 * ivdw=0 no vdw interaction
 * ivdw=1 standard L-J interaction
 * ivdw=2 Buckingham
 * ivdw=3 tabulated vdw.
 *
 * Kind of ugly, but it works.
 */
static void init_nblist(t_nblist *nl_sr,t_nblist *nl_lr,
                        int maxsr,int maxlr,
                        int ivdw, int icoul, 
                        bool bfree, int solvent, int nltype)
{
    t_nblist *nl;
    int      homenr;
    int      i,nn;
    
    int inloop[20] =
    { 
        eNR_NBKERNEL_NONE,
        eNR_NBKERNEL010,
        eNR_NBKERNEL020,
        eNR_NBKERNEL030,
        eNR_NBKERNEL100,
        eNR_NBKERNEL110,
        eNR_NBKERNEL120,
        eNR_NBKERNEL130,
        eNR_NBKERNEL200,
        eNR_NBKERNEL210,
        eNR_NBKERNEL220,
        eNR_NBKERNEL230,
        eNR_NBKERNEL300,
        eNR_NBKERNEL310,
        eNR_NBKERNEL320,
        eNR_NBKERNEL330,
        eNR_NBKERNEL400,
        eNR_NBKERNEL410,
        eNR_NBKERNEL_NONE,
        eNR_NBKERNEL430
    };
  
    for(i=0; (i<2); i++)
    {
        nl     = (i == 0) ? nl_sr : nl_lr;
        homenr = (i == 0) ? maxsr : maxlr;
        
        /* Set coul/vdw in neighborlist, and for the normal loops we determine
         * an index of which one to call.
         * If the index is -1 we use generic loops, or free energy loops (both slower).
         */
        nl->ivdw  = ivdw;
        nl->icoul = icoul;
    
        if(bfree)
        {
            nl->il_code = -1;
        }
        else
        {
            /* Ewald uses the standard tabulated loops */
            if(icoul==4)
            {
                icoul==3;
            }
            nn = inloop[4*icoul*ivdw];
            
            /* solvent loops follow directly after the corresponding
            * ordinary loops, in the order:
            *
            * SPC, SPC-SPC, TIP4p, TIP4p-TIP4p
            *   
            */
            if(solvent == esolSPC)
            {
                if(nltype == enlistWATER)
                    nn += 1;
                else if (nltype == enlistWATERWATER)
                    nn += 2;
            } 
            else if(solvent == esolTIP4P)
            {
                if(nltype == enlistWATER)
                    nn += 3;
                else if (nltype == enlistWATERWATER)
                    nn += 4;      
            }
            
            nl->il_code = nn;
        }

        if (debug)
            fprintf(debug,"Initiating neighbourlist type %d for %s interactions,\nwith %d SR, %d LR atoms.\n",
                    nl->il_code,ENLISTTYPE(solvent),maxsr,maxlr);
        
        /* maxnri is influenced by the number of shifts (maximum is 8)
         * and the number of energy groups.
         * If it is not enough, nl memory will be reallocated during the run.
         * 4 seems to be a reasonable factor, which only causes reallocation
         * during runs with tiny and many energygroups.
         */
        nl->maxnri      = homenr*4;
        nl->maxnrj      = 0;
        nl->maxlen      = 0;
        nl->nri         = 0;
        nl->nrj         = 0;
        nl->iinr        = NULL;
        nl->gid         = NULL;
        nl->shift       = NULL;
        nl->jindex      = NULL;
        nl->solvent_opt = solvent;
        reallocate_nblist(nl);
        nl->jindex[0] = 0;
        nl->jindex[1] = 0;
        nl->gid[0] = -1;
#ifdef GMX_THREADS
        nl->counter = 0;
        snew(nl->mtx,1);
        pthread_mutex_init(nl->mtx,NULL);
#endif
    }
}



static int correct_box_elem(tensor box,int v,int d)
{
  int shift;

  shift = 0;

  /* correct elem d of vector v with vector d */
  while (box[v][d] > BOX_MARGIN*box[d][d]) {
    fprintf(stdlog,"Correcting invalid box:\n");
    pr_rvecs(stdlog,0,"old box",box,DIM);
    rvec_dec(box[v],box[d]);
    shift--;
    pr_rvecs(stdlog,0,"new box",box,DIM);
  } 
  while (-box[v][d] > BOX_MARGIN*box[d][d]) {
    fprintf(stdlog,"Correcting invalid box:\n");
    pr_rvecs(stdlog,0,"old box",box,DIM);
    rvec_inc(box[v],box[d]);
    shift++;
    pr_rvecs(stdlog,0,"new box",box,DIM);
  }

  return shift;
}

void correct_box(tensor box,t_forcerec *fr,t_graph *g)
{
  int zy,zx,yx,x,y,z,shift,nl,l,i;

  /* check if the box still obeys the restrictions, if not, correct it */
  zy = correct_box_elem(box,ZZ,YY);
  zx = correct_box_elem(box,ZZ,XX);
  yx = correct_box_elem(box,YY,XX);
  
  if (zy || zx || yx) {
    /* correct the graph */
    if (g) {
      for(i=0; i<g->nnodes; i++) {
	g->ishift[i][YY] -= g->ishift[i][ZZ]*zy;
	g->ishift[i][XX] -= g->ishift[i][ZZ]*zx;
	g->ishift[i][XX] -= g->ishift[i][YY]*yx;
      }
    }
    /* correct the shift indices of the short-range neighborlists */
    for(nl=0; nl<fr->nnblists; nl++)
      for(l=0; l<eNL_NR; l++)
	for(i=0; i<fr->nblists[nl].nlist_sr[l].nri; i++) {
	  shift = fr->nblists[nl].nlist_sr[l].shift[i];
	  x = IS2X(shift);
	  y = IS2Y(shift);
	  z = IS2Z(shift);
	  y -= z*zy;
	  x -= z*zx;
	  x -= y*yx;
	  shift = XYZ2IS(x,y,z);
	  if (shift<0 || shift>=SHIFTS)
	  gmx_fatal(FARGS,"Could not correct too skewed box");
	  fr->nblists[nl].nlist_sr[l].shift[i] = shift;
	}
  }
}

void init_neighbor_list(FILE *log,t_forcerec *fr,int homenr)
{
   /* Make maxlr tunable! (does not seem to be a big difference though) 
    * This parameter determines the number of i particles in a long range 
    * neighbourlist. Too few means many function calls, too many means
    * cache trashing.
    */
   int maxsr,maxsr_wat,maxlr,maxlr_wat;
   int icoul, ivdw;
   int solvent;
   int i;
   t_nblists *nbl;

   /* maxsr     = homenr-fr->nWatMol*3; */
   maxsr     = homenr;

   if (maxsr < 0)
     gmx_fatal(FARGS,"%s, %d: Negative number of short range atoms.\n"
		 "Call your Gromacs dealer for assistance.",__FILE__,__LINE__);
   maxsr_wat = fr->nWatMol; 
   if (fr->bTwinRange) 
   {
       maxlr     = 50;
       maxlr_wat = min(fr->nWatMol,maxlr);
   }
   else {
     maxlr = maxlr_wat = 0;
   }  

   solvent = fr->solvent_opt;

   /* Determine the values for icoul/ivdw. */
   if(fr->bcoultab)
   {
       if(fr->bEwald)
           icoul = 4;
       else
           icoul = 3;
   }
   else if(EEL_RF(fr->eeltype))
   {
       icoul = 2;
   }
   else 
   {
       icoul = 1;
   }
   
   if(fr->bvdwtab)
   {
       ivdw = 3;
   }
   else if(fr->bBHAM)
   {
       ivdw = 2;
   }
   else 
   {
       ivdw = 1;
   }
   
   for(i=0; i<fr->nnblists; i++) 
   {
       nbl = &(fr->nblists[i]);
       init_nblist(&nbl->nlist_sr[eNL_VDWQQ],&nbl->nlist_lr[eNL_VDWQQ],
                   maxsr,maxlr,ivdw,icoul,FALSE,solvent,enlistATOM);
       init_nblist(&nbl->nlist_sr[eNL_VDW],&nbl->nlist_lr[eNL_VDW],
                   maxsr,maxlr,ivdw,0,FALSE,solvent,enlistATOM);
       init_nblist(&nbl->nlist_sr[eNL_QQ],&nbl->nlist_lr[eNL_QQ],
                   maxsr,maxlr,0,icoul,FALSE,solvent,enlistATOM);
       init_nblist(&nbl->nlist_sr[eNL_VDWQQ_WATER],&nbl->nlist_lr[eNL_VDWQQ_WATER],
                   maxsr_wat,maxlr_wat,ivdw,icoul, FALSE,solvent,enlistWATER);
       init_nblist(&nbl->nlist_sr[eNL_QQ_WATER],&nbl->nlist_lr[eNL_QQ_WATER],
                   maxsr_wat,maxlr_wat,0,icoul, FALSE,solvent,enlistWATER);
       init_nblist(&nbl->nlist_sr[eNL_VDWQQ_WATERWATER],&nbl->nlist_lr[eNL_VDWQQ_WATERWATER],
                   maxsr_wat,maxlr_wat,ivdw,icoul, FALSE,solvent,enlistWATERWATER);
       init_nblist(&nbl->nlist_sr[eNL_QQ_WATERWATER],&nbl->nlist_lr[eNL_QQ_WATERWATER],
                   maxsr_wat,maxlr_wat,0,icoul, FALSE,solvent,enlistWATERWATER);
       
       if (fr->efep != efepNO) 
       {
           init_nblist(&nbl->nlist_sr[eNL_VDWQQ_FREE],&nbl->nlist_lr[eNL_VDWQQ_FREE],
                       maxsr,maxlr,ivdw,icoul, TRUE,solvent,enlistATOM);
           init_nblist(&nbl->nlist_sr[eNL_VDW_FREE],&nbl->nlist_lr[eNL_VDW_FREE],
                       maxsr,maxlr,ivdw,0,TRUE,solvent,enlistATOM);
           init_nblist(&nbl->nlist_sr[eNL_QQ_FREE],&nbl->nlist_lr[eNL_QQ_FREE],
                       maxsr,maxlr,0,icoul, TRUE,solvent,enlistATOM);
       }  
   }
}

 static void reset_nblist(t_nblist *nl)
 {
   nl->nri       = 0;
   nl->nrj       = 0;
   nl->maxlen    = 0;
   if (nl->maxnri > 0) {
     nl->gid[0]   = -1;
     if (nl->maxnrj > 1) {
       nl->jindex[0] = 0;
       nl->jindex[1] = 0;
     }
   }
}

static void reset_neighbor_list(t_forcerec *fr,bool bLR,int nls,int eNL)
{
  int n,i;
  
  if (bLR) 
    reset_nblist(&(fr->nblists[nls].nlist_lr[eNL]));
  else {
    for(n=0; n<fr->nnblists; n++)
      for(i=0; i<eNL_NR; i++)
	reset_nblist(&(fr->nblists[n].nlist_sr[i]));
  }
}

static inline void new_i_nblist(t_nblist *nlist,
				bool bLR,atom_id i_atom,int shift,int gid)
{
  int    i,k,nri,nshift;
    
  if (nlist->maxnrj <= nlist->nrj + NLJ_INC-1) {
    if (debug)
      fprintf(debug,"Adding %5d J particles for %s nblist %s\n",NLJ_INC,
	      bLR ? "LR" : "SR",nrnb_str(nlist->il_code));

    nlist->maxnrj += NLJ_INC;
    srenew(nlist->jjnr,nlist->maxnrj);
  }

  nri = nlist->nri;

  /* Check whether we have to increase the i counter */
  if ((nlist->iinr[nri]  != i_atom) || 
      (nlist->shift[nri] != shift) || 
      (nlist->gid[nri]   != gid)) {
    /* This is something else. Now see if any entries have 
     * been added in the list of the previous atom.
     */
    if ((nlist->jindex[nri+1] > nlist->jindex[nri]) && 
	(nlist->gid[nri] != -1)) {
      
      /* If so increase the counter */
      nlist->nri++;
      nri++;
      if (nlist->nri >= nlist->maxnri) {
	nlist->maxnri += NLI_INC;
	reallocate_nblist(nlist);
      }
    }
    /* Set the number of neighbours and the atom number */
    nlist->jindex[nri+1] = nlist->jindex[nri];
    nlist->iinr[nri]     = i_atom;
    nlist->gid[nri]      = gid;
    nlist->shift[nri]    = shift;
  }
}

#ifdef SORTNLIST
#define aswap(v,i,j) {  \
  atom_id temp;         \
                        \
  temp=v[i];            \
  v[i]=v[j];            \
  v[j]=temp;            \
}

static void quicksort(atom_id v[], int left, int right)
{
  int i,last;

  if (left >= right)                    /* Do nothing if array contains */
    return;                             /* fewer than two elements      */
  aswap(v,left,(left+right)/2);         /* Move partition element       */
  last=left;                            /* to v[0]                      */
  for(i=left+1; (i<=right); i++)        /* partition                    */
    if (v[i] < v[left]) {
      last++;
      aswap(v,last,i);                  /* watch out for macro trick    */
    }
  aswap(v,left,last);                   /* restore partition element    */
  quicksort(v,left,last-1);
  quicksort(v,last+1,right);
}
#endif

static inline void close_i_nblist(t_nblist *nlist) 
{
  int nri = nlist->nri;
  int len;
  
  nlist->jindex[nri+1] = nlist->nrj;

  len=nlist->nrj -  nlist->jindex[nri];
  
  /* nlist length for water i molecules is treated statically 
   * in the innerloops 
   */
  if(len > nlist->maxlen)
    nlist->maxlen = len;
}

static inline void close_nblist(t_nblist *nlist)
{
  if (nlist->maxnri > 0) {
    int nri = nlist->nri;
    
    if ((nlist->jindex[nri+1] > nlist->jindex[nri]) && 
	(nlist->gid[nri] != -1)) {
      nlist->nri++;
      nlist->jindex[nri+2] = nlist->nrj;
    }
  }
}

static inline void close_neighbor_list(t_forcerec *fr,bool bLR,int nls,int eNL)
{
  int n,i;

  if (bLR)
    close_nblist(&(fr->nblists[nls].nlist_lr[eNL]));
  else {
    for(n=0; n<fr->nnblists; n++)
      for(i=0; (i<eNL_NR); i++) 
	close_nblist(&(fr->nblists[n].nlist_sr[i]));
  }
}

static void add_j_to_nblist(t_nblist *nlist,atom_id j_atom)
{
  int nrj=nlist->nrj;
  
  nlist->jjnr[nrj] = j_atom;
  nlist->nrj ++;
}

static inline void 
put_in_list(bool          bHaveVdW[],
	    int               ngid,
	    t_mdatoms *       md,
	    int               icg,
	    int               jgid,
	    int               nj,
	    atom_id           jjcg[],
	    atom_id           index[],
	    t_excl            bExcl[],
	    int               shift,
	    t_forcerec *      fr,
	    bool              bLR,
	    bool              bDoVdW,
	    bool              bDoCoul)
{
  /* The a[] index has been removed,
   * to put it back in i_atom should be a[i0] and jj should be a[jj].
   */
  t_nblist *   vdwc;
  t_nblist *   vdw;
  t_nblist *   coul;
  t_nblist *   vdwc_free  = NULL;
  t_nblist *   vdw_free   = NULL;
  t_nblist *   coul_free  = NULL;
  t_nblist *   vdwc_ww    = NULL;
  t_nblist *   coul_ww    = NULL;
  
  int 	    i,j,jcg,igid,gid,nbl_ind,ind_ij;
  atom_id   jj,jj0,jj1,i_atom;
  int       i0,nicg,len;
  
  int       *type,*typeB;
  unsigned short    *cENER;
  real      *charge,*chargeB;
  real      qi,qiB,qq,rlj;
  bool      bFreeEnergy,bFree,bFreeJ,bNotEx,*bPert;
  bool      bDoVdW_i,bDoCoul_i;
  int       iwater,jwater;
  t_nblist  *nlist;

#ifdef SORTNLIST
  /* Quicksort the charge groups in the neighbourlist to obtain
   * better caching properties. We do this only for the short range, 
   * i.e. when we use the nlist more than once
   */
  
  if (!bLR) 
    quicksort(jjcg,0,nj-1);
#endif
  
  /* Copy some pointers */
  charge  = md->chargeA;
  chargeB = md->chargeB;
  type    = md->typeA;
  typeB   = md->typeB;
  cENER   = md->cENER;
  bPert   = md->bPerturbed;
  
  /* Check whether this molecule is a water molecule */
  i0     = index[icg];
  nicg   = index[icg+1]-i0;
  
  iwater = fr->solvent_type[icg];
  
  bFreeEnergy = FALSE;
  if (md->nPerturbed) 
    {
      /* Check if any of the particles involved are perturbed. 
       * If not we can do the cheaper normal put_in_list
       * and use more solvent optimization.
       */
      for(i=0; i<nicg; i++)
	bFreeEnergy |= bPert[i0+i];
      /* Loop over the j charge groups */
      for(j=0; (j<nj && !bFreeEnergy); j++) 
	{
	  jcg = jjcg[j];
	  jj0 = index[jcg];
	  jj1 = index[jcg+1];
	  /* Finally loop over the atoms in the j-charge group */	
	  for(jj=jj0; jj<jj1; jj++)
	    bFreeEnergy |= bPert[jj];
	}
    }

  /* Unpack pointers to neighbourlist structs */
  if (fr->nnblists == 1) {
    nbl_ind = 0;
  } else {
    igid    = cENER[i0];
    nbl_ind = fr->gid2nblists[GID(igid,jgid,ngid)];
  }
  if (bLR)
    nlist = fr->nblists[nbl_ind].nlist_lr;
  else
    nlist = fr->nblists[nbl_ind].nlist_sr;
      
  if (iwater != esolNO) {
    vdwc = &nlist[eNL_VDWQQ_WATER];
    vdw  = &nlist[eNL_VDW];
    coul = &nlist[eNL_QQ_WATER];
#ifndef DISABLE_WATERWATER_NLIST
    vdwc_ww = &nlist[eNL_VDWQQ_WATERWATER];
    coul_ww = &nlist[eNL_QQ_WATERWATER];
#endif
  } else {
    vdwc = &nlist[eNL_VDWQQ];
    vdw  = &nlist[eNL_VDW];
    coul = &nlist[eNL_QQ];
  }
  
  if (!bFreeEnergy) 
    {
      if (iwater != esolNO) 
	{
	  /* Loop over the atoms in the i charge group */    
	  i_atom  = i0;
	  igid    = cENER[i_atom];
	  gid     = GID(igid,jgid,ngid);
	  /* Create new i_atom for each energy group */
	  if (bDoCoul && bDoVdW)
	    {
	      new_i_nblist(vdwc,bLR,i_atom,shift,gid);
#ifndef DISABLE_WATERWATER_NLIST
	      new_i_nblist(vdwc_ww,bLR,i_atom,shift,gid);
#endif
	    }
	  if (bDoVdW)
	    new_i_nblist(vdw,bLR,i_atom,shift,gid);
	  if (bDoCoul) 
	    {
	      new_i_nblist(coul,bLR,i_atom,shift,gid);
#ifndef DISABLE_WATERWATER_NLIST
	      new_i_nblist(coul_ww,bLR,i_atom,shift,gid);
#endif
	    }      
	  /* Loop over the j charge groups */
	  for(j=0; (j<nj); j++) 
	    {
	      jcg=jjcg[j];
	      
	      if (jcg==icg)
		continue;
	      
	      jj0 = index[jcg];
	      jwater = fr->solvent_type[jcg];
	      
	      if (iwater == esolSPC && jwater == esolSPC)
		{
		  /* Interaction between two SPC molecules */
		  if (!bDoCoul)
		    {
		      /* VdW only - only first atoms in each water interact */
		      add_j_to_nblist(vdw,jj0);
		    }
		  else 
		    {
#ifdef DISABLE_WATERWATER_NLIST	
		      /* Add entries for the three atoms - only do VdW if we need to */
		      if (!bDoVdW)
			add_j_to_nblist(coul,jj0);
		      else
			add_j_to_nblist(vdwc,jj0);
		      
		      add_j_to_nblist(coul,jj0+1);
		      add_j_to_nblist(coul,jj0+2);	    
#else
		      /* One entry for the entire water-water interaction */
		      if (!bDoVdW)
			add_j_to_nblist(coul_ww,jj0);
		      else
			add_j_to_nblist(vdwc_ww,jj0);
#endif
		    }  
		} 
	      else if(iwater == esolTIP4P && jwater == esolTIP4P) 
		{
		  /* Interaction between two TIP4p molecules */
		  if (!bDoCoul)
		    {
		      /* VdW only - only first atoms in each water interact */
		      add_j_to_nblist(vdw,jj0);
		    }
		  else 
		    {
#ifdef DISABLE_WATERWATER_NLIST	
		      /* Add entries for the four atoms - only do VdW if we need to */
		      if (bDoVdW)
			add_j_to_nblist(vdw,jj0);
		      
		      add_j_to_nblist(coul,jj0+1);
		      add_j_to_nblist(coul,jj0+2);	    
		      add_j_to_nblist(coul,jj0+3);	    
#else
		      /* One entry for the entire water-water interaction */
		      if (!bDoVdW)
			add_j_to_nblist(coul_ww,jj0);
		      else
			add_j_to_nblist(vdwc_ww,jj0);
#endif
		    }  					
		}
	      else 
		{
		  /* j charge group is not water, but i is.
		   * Add entries to the water-other_atom lists; the geometry of the water
		   * molecule doesn't matter - that is taken care of in the nonbonded kernel,
		   * so we don't care if it is SPC or TIP4P...
		   */
		  
		  jj1 = index[jcg+1];
		  
		  if (!bDoVdW) 
		    {
		      for(jj=jj0; (jj<jj1); jj++) 
			{
			  if (charge[jj] != 0)
                            {
			      add_j_to_nblist(coul,jj);
                            }
			}
		    }
		  else if (!bDoCoul)
		    {
		      for(jj=jj0; (jj<jj1); jj++) 
			if (bHaveVdW[type[jj]])
			  add_j_to_nblist(vdw,jj);
		    }
		  else 
		    {
		      /* _charge_ _groups_ interact with both coulomb and LJ */
		      /* Check which atoms we should add to the lists!       */
		      for(jj=jj0; (jj<jj1); jj++) 
			{
			  if (bHaveVdW[type[jj]]) 
			    {
			      if (charge[jj] != 0)
                                {
				  add_j_to_nblist(vdwc,jj);
				}
			      else
				{
				  add_j_to_nblist(vdw,jj);
                                }
                            }
			  else if (charge[jj] != 0)
                            {
			      add_j_to_nblist(coul,jj);
			    }
			}
		    }
		}
	    }
	  close_i_nblist(vdw); 
	  close_i_nblist(coul); 
	  close_i_nblist(vdwc);  
#ifndef DISABLE_WATERWATER_NLIST
	  close_i_nblist(coul_ww);
	  close_i_nblist(vdwc_ww); 
#endif
	} 
      else
	{ 
	  /* no solvent as i charge group */
	  /* Loop over the atoms in the i charge group */    
	  for(i=0; i<nicg; i++) 
	    {
	      i_atom  = i0+i;
	      igid    = cENER[i_atom];
	      gid     = GID(igid,jgid,ngid);
	      qi      = charge[i_atom];
	      
	      /* Create new i_atom for each energy group */
	      if (bDoVdW && bDoCoul) 
		new_i_nblist(vdwc,bLR,i_atom,shift,gid);
	      
	      if (bDoVdW) 
		new_i_nblist(vdw,bLR,i_atom,shift,gid);
	      if (bDoCoul) 
		new_i_nblist(coul,bLR,i_atom,shift,gid);
	      
	      bDoVdW_i  = (bDoVdW  && bHaveVdW[type[i_atom]]);
	      bDoCoul_i = (bDoCoul && qi!=0);
	      
	      if (bDoVdW_i || bDoCoul_i) 
		{
		  /* Loop over the j charge groups */
		  for(j=0; (j<nj); j++) 
		    {
		      jcg=jjcg[j];
		      
		      /* Check for large charge groups */
		      if (jcg == icg) 
			jj0 = i0 + i + 1;
		      else 
			jj0 = index[jcg];
		      
		      jj1=index[jcg+1];
		      /* Finally loop over the atoms in the j-charge group */	
		      for(jj=jj0; jj<jj1; jj++) 
			{
			  bNotEx = NOTEXCL(bExcl,i,jj);
			  
			  if (bNotEx) 
			    {
			      if (!bDoVdW_i) 
				{ 
				  if (charge[jj] != 0)
				    add_j_to_nblist(coul,jj);
				}
			      else if (!bDoCoul_i) 
				{
				  if (bHaveVdW[type[jj]])
				    add_j_to_nblist(vdw,jj);
				}
			      else 
				{
				  if (bHaveVdW[type[jj]]) 
				    {
				      if (charge[jj] != 0)
					add_j_to_nblist(vdwc,jj);
				      else
					add_j_to_nblist(vdw,jj);
				    } 
				  else if (charge[jj] != 0)
				    {
				      add_j_to_nblist(coul,jj);
				    }
				}
			    }
			}
		    }
		}
	      close_i_nblist(vdw);
	      close_i_nblist(coul);
	      close_i_nblist(vdwc);
	    }
	}
    }
  else
    {
      /* we are doing free energy */
      vdwc_free = &nlist[eNL_VDWQQ_FREE];
      vdw_free  = &nlist[eNL_VDW_FREE];
      coul_free = &nlist[eNL_QQ_FREE];
      /* Loop over the atoms in the i charge group */    
      for(i=0; i<nicg; i++) 
	{
	  i_atom  = i0+i;
	  igid    = cENER[i_atom];
	  gid     = GID(igid,jgid,ngid);
	  qi      = charge[i_atom];
	  qiB     = chargeB[i_atom];
	  
	  /* Create new i_atom for each energy group */
	  if (bDoVdW && bDoCoul) 
	    new_i_nblist(vdwc,bLR,i_atom,shift,gid);
	  if (bDoVdW)   
	    new_i_nblist(vdw,bLR,i_atom,shift,gid);
	  if (bDoCoul) 
	    new_i_nblist(coul,bLR,i_atom,shift,gid);
	  
	  new_i_nblist(vdw_free,bLR,i_atom,shift,gid);
	  new_i_nblist(coul_free,bLR,i_atom,shift,gid);
	  new_i_nblist(vdwc_free,bLR,i_atom,shift,gid);
	  
	  bDoVdW_i  = (bDoVdW  &&
		       (bHaveVdW[type[i_atom]] || bHaveVdW[typeB[i_atom]]));
	  bDoCoul_i = (bDoCoul && (qi!=0 || qiB!=0));
	  
	  if (bDoVdW_i || bDoCoul_i) 
	    {
	      /* Loop over the j charge groups */
	      for(j=0; (j<nj); j++)
		{
		  jcg=jjcg[j];
		  
		  /* Check for large charge groups */
		  if (jcg == icg) 
		    jj0 = i0 + i + 1;
		  else 
		    jj0 = index[jcg];
		  
		  jj1=index[jcg+1];
		  /* Finally loop over the atoms in the j-charge group */	
		  bFree = bPert[i_atom];
		  for(jj=jj0; (jj<jj1); jj++) 
		    {
		      bFreeJ = bFree || bPert[jj];
		      /* Complicated if, because the water H's should also
		       * see perturbed j-particles
		       */
		      if (iwater==esolNO || i==0 || bFreeJ) 
			{
			  bNotEx = NOTEXCL(bExcl,i,jj);
			  
			  if (bNotEx) 
			    {
			      if (bFreeJ)
				{
				  if (!bDoVdW_i) 
				    {
				      if (charge[jj]!=0 || chargeB[jj]!=0)
					add_j_to_nblist(coul_free,jj);
				    }
				  else if (!bDoCoul_i) 
				    {
				      if (bHaveVdW[type[jj]] || bHaveVdW[typeB[jj]])
					add_j_to_nblist(vdw_free,jj);
				    }
				  else 
				    {
				      if (bHaveVdW[type[jj]] || bHaveVdW[typeB[jj]]) 
					{
					  if (charge[jj]!=0 || chargeB[jj]!=0)
					    add_j_to_nblist(vdwc_free,jj);
					  else
					    add_j_to_nblist(vdw_free,jj);
					}
				      else if (charge[jj]!=0 || chargeB[jj]!=0)
					add_j_to_nblist(coul_free,jj);
				    }
				}
			      else if (!bDoVdW_i) 
				{ 
				  /* This is done whether or not bWater is set */
				  if (charge[jj] != 0)
				    add_j_to_nblist(coul,jj);
				}
			      else if (!bDoCoul_i) 
				{ 
				  if (bHaveVdW[type[jj]])
				    add_j_to_nblist(vdw,jj);
				}
			      else 
				{
				  if (bHaveVdW[type[jj]]) 
				    {
				      if (charge[jj] != 0)
					add_j_to_nblist(vdwc,jj);
				      else
					add_j_to_nblist(vdw,jj);
				    } 
				  else if (charge[jj] != 0)
				    add_j_to_nblist(coul,jj);
				}
			    }
			}
		    }
		}
	    }
	  close_i_nblist(vdw);
	  close_i_nblist(coul);
	  close_i_nblist(vdwc);
	  close_i_nblist(vdw_free);
	  close_i_nblist(coul_free);
	  close_i_nblist(vdwc_free);
	}
    }
}

/*
static void setexcl(int nri,atom_id ia[],t_block *excl,bool b,
	            t_excl bexcl[])
{
  atom_id k;
  int     i,inr;
  
  if (b) {
    for(i=0; (i<nri); i++) {
      inr = ia[i];
      for(k=excl->index[inr]; (k<excl->index[inr+1]); k++) {
	SETEXCL(bexcl,i,excl->a[k]);
      }
    }
  }
  else {
    for(i=0; (i<nri); i++) {
      inr = ia[i];
      for(k=excl->index[inr]; (k<excl->index[inr+1]); k++) {
	RMEXCL(bexcl,i,excl->a[k]);
      }
    }
  }
}
*/

static void setexcl(atom_id start,atom_id end,t_block *excl,bool b,
		    t_excl bexcl[])
{
  atom_id i,k;
  
  if (b) {
    for(i=start; i<end; i++) {
      for(k=excl->index[i]; k<excl->index[i+1]; k++) {
	SETEXCL(bexcl,i-start,excl->a[k]);
      }
    }
  }
  else {
    for(i=start; i<end; i++) {
      for(k=excl->index[i]; k<excl->index[i+1]; k++) {
	RMEXCL(bexcl,i-start,excl->a[k]);
      }
    }
  }
}

int calc_naaj(int icg,int cgtot)
{
  int naaj;
  
  if ((cgtot % 2) == 1) {
    /* Odd number of charge groups, easy */
    naaj = 1+(cgtot/2);
  }
  else if ((cgtot % 4) == 0) {
    /* Multiple of four is hard */
    if (icg < cgtot/2) {
      if ((icg % 2) == 0)
	naaj=1+(cgtot/2);
      else
	naaj=cgtot/2;
    }
    else {
      if ((icg % 2) == 1)
	naaj=1+(cgtot/2);
      else
	naaj=cgtot/2;
    }
  }
  else {
    /* cgtot/2 = odd */
    if ((icg % 2) == 0)
      naaj=1+(cgtot/2);
    else
      naaj=cgtot/2;
  }
#ifdef DEBUG
  fprintf(log,"naaj=%d\n",naaj);
#endif
  return naaj;
}

/************************************************
 *
 *  S I M P L E      C O R E     S T U F F
 *
 ************************************************/

static real calc_image_tric(rvec xi,rvec xj,matrix box,
			    rvec b_inv,int *shift)
{
  /* This code assumes that the cut-off is smaller than
   * a half times the smallest diagonal element of the box.
   */
  const real h25=2.5;
  real dx,dy,dz;
  real r2;
  int  tx,ty,tz;
  
  /* Compute diff vector */
  dz=xj[ZZ]-xi[ZZ];
  dy=xj[YY]-xi[YY];
  dx=xj[XX]-xi[XX];
  
  /* Perform NINT operation, using trunc operation, therefore
   * we first add 2.5 then subtract 2 again
   */
  tz=dz*b_inv[ZZ]+h25;
  tz-=2;
  dz-=tz*box[ZZ][ZZ];
  dy-=tz*box[ZZ][YY];
  dx-=tz*box[ZZ][XX];

  ty=dy*b_inv[YY]+h25;
  ty-=2;
  dy-=ty*box[YY][YY];
  dx-=ty*box[YY][XX];

  tx=dx*b_inv[XX]+h25;
  tx-=2;
  dx-=tx*box[XX][XX];
  
  /* Distance squared */
  r2=(dx*dx)+(dy*dy)+(dz*dz);

  *shift=XYZ2IS(tx,ty,tz);

  return r2;
}

static real calc_image_rect(rvec xi,rvec xj,rvec box_size,
			    rvec b_inv,int *shift)
{
  const real h15=1.5;
  real ddx,ddy,ddz;
  real dx,dy,dz;
  real r2;
  int  tx,ty,tz;
  
  /* Compute diff vector */
  dx=xj[XX]-xi[XX];
  dy=xj[YY]-xi[YY];
  dz=xj[ZZ]-xi[ZZ];
  
  /* Perform NINT operation, using trunc operation, therefore
   * we first add 1.5 then subtract 1 again
   */
  tx=dx*b_inv[XX]+h15;
  ty=dy*b_inv[YY]+h15;
  tz=dz*b_inv[ZZ]+h15;
  tx--;
  ty--;
  tz--;
  
  /* Correct diff vector for translation */
  ddx=tx*box_size[XX]-dx;
  ddy=ty*box_size[YY]-dy;
  ddz=tz*box_size[ZZ]-dz;
  
  /* Distance squared */
  r2=(ddx*ddx)+(ddy*ddy)+(ddz*ddz);
  
  *shift=XYZ2IS(tx,ty,tz);

  return r2;
}

static void add_simple(t_ns_buf *nsbuf,int nrj,atom_id cg_j,
		       bool bHaveVdW[],int ngid,t_mdatoms *md,
		       int icg,int jgid,t_block *cgs,t_excl bexcl[],
		       int shift,t_forcerec *fr)
{
  if (nsbuf->ncg >= MAX_CG) {
    put_in_list(bHaveVdW,ngid,md,icg,jgid,nsbuf->ncg,nsbuf->jcg,
		cgs->index,/* cgs->a, */ bexcl,shift,fr,FALSE,TRUE,TRUE);
    /* Reset buffer contents */
    nsbuf->ncg = nsbuf->nj = 0;
  }
  nsbuf->jcg[nsbuf->ncg++]=cg_j;
  nsbuf->nj += nrj;
}

static void ns_inner_tric(rvec x[],int icg,bool *i_egp_flags,
			  int njcg,atom_id jcg[],
			  matrix box,rvec b_inv,real rcut2,
			  t_block *cgs,t_ns_buf **ns_buf,unsigned short gid[],
			  bool bHaveVdW[],int ngid,t_mdatoms *md,
			  t_excl bexcl[],t_forcerec *fr)
{
  int      shift;
  int      j,nrj,jgid;
  atom_id  cg_j,*cgindex /* ,*cga */;
  t_ns_buf *nsbuf;
  
  cgindex = cgs->index;
  /* cga     = cgs->a; */
  shift   = CENTRAL;
  for(j=0; (j<njcg); j++) {
    cg_j   = jcg[j];
    nrj    = cgindex[cg_j+1]-cgindex[cg_j];
    if (calc_image_tric(x[icg],x[cg_j],box,b_inv,&shift) < rcut2) {
      /* jgid  = gid[cga[cgindex[cg_j]]]; */
      jgid  = gid[cgindex[cg_j]];
      if (!(i_egp_flags[jgid] & EGP_EXCL)) {
	add_simple(&ns_buf[jgid][shift],nrj,cg_j,
		   bHaveVdW,ngid,md,icg,jgid,cgs,bexcl,shift,fr);
      }
    }
  }
}

static void ns_inner_rect(rvec x[],int icg,bool *i_egp_flags,
			  int njcg,atom_id jcg[],
			  bool bBox,rvec box_size,rvec b_inv,real rcut2,
			  t_block *cgs,t_ns_buf **ns_buf,unsigned short gid[],
			  bool bHaveVdW[],int ngid,t_mdatoms *md,
			  t_excl bexcl[],t_forcerec *fr)
{
  int      shift;
  int      j,nrj,jgid;
  atom_id  cg_j,*cgindex /* ,*cga */;
  t_ns_buf *nsbuf;

  cgindex = cgs->index;
  /* cga     = cgs->a; */
  if (bBox) {
    shift = CENTRAL;
    for(j=0; (j<njcg); j++) {
      cg_j   = jcg[j];
      nrj    = cgindex[cg_j+1]-cgindex[cg_j];
      if (calc_image_rect(x[icg],x[cg_j],box_size,b_inv,&shift) < rcut2) {
	/* jgid  = gid[cga[cgindex[cg_j]]]; */
	jgid  = gid[cgindex[cg_j]];
	if (!(i_egp_flags[jgid] & EGP_EXCL)) {
	  add_simple(&ns_buf[jgid][shift],nrj,cg_j,
		     bHaveVdW,ngid,md,icg,jgid,cgs,bexcl,shift,fr);
	}
      }
    }
  } 
  else {
    for(j=0; (j<njcg); j++) {
      cg_j   = jcg[j];
      nrj    = cgindex[cg_j+1]-cgindex[cg_j];
      if ((rcut2 == 0) || (distance2(x[icg],x[cg_j]) < rcut2)) {
	/* jgid  = gid[cga[cgindex[cg_j]]]; */
	jgid  = gid[cgindex[cg_j]];
	if (!(i_egp_flags[jgid] & EGP_EXCL)) {
	  add_simple(&ns_buf[jgid][CENTRAL],nrj,cg_j,
		     bHaveVdW,ngid,md,icg,jgid,cgs,bexcl,CENTRAL,fr);
	}
      }
    }
  }
}

static int ns_simple_core(t_forcerec *fr,
			  t_topology *top,
			  t_mdatoms *md,
			  matrix box,rvec box_size,
			  t_excl bexcl[],
			  int ngid,t_ns_buf **ns_buf,
			  bool bHaveVdW[])
{
  static   atom_id  *aaj=NULL;
  int      naaj,k;
  real     rlist2;
  int      nsearch,icg,jcg,i0,nri,nn;
  t_ns_buf *nsbuf;
  /* atom_id  *i_atoms; */
  t_block  *cgs=&(top->blocks[ebCGS]);
  t_block  *excl=&(top->atoms.excl);
  rvec     b_inv;
  int      m;
  bool     bBox,bTriclinic,*i_egp_flags;
  
  if (aaj==NULL) {
    snew(aaj,2*cgs->nr);
    for(jcg=0; (jcg<cgs->nr); jcg++) {
      aaj[jcg]=jcg;
      aaj[jcg+cgs->nr]=jcg;
    }
  }
  rlist2 = sqr(fr->rlist);

  bBox = (fr->ePBC != epbcNONE);
  if (bBox) {
    for(m=0; (m<DIM); m++)
      b_inv[m]=divide(1.0,box_size[m]);
    bTriclinic = TRICLINIC(box);
  } else
    bTriclinic = FALSE;

  nsearch=0;
  for (icg=fr->cg0; (icg<fr->hcg); icg++) {
    /*
    i0        = cgs->index[icg];
    nri       = cgs->index[icg+1]-i0;
    i_atoms   = &(cgs->a[i0]);
    i_eg_excl = fr->eg_excl + ngid*md->cENER[*i_atoms];
    setexcl(nri,i_atoms,excl,TRUE,bexcl);
    */
    i_egp_flags = fr->egp_flags + ngid*md->cENER[cgs->index[icg]];
    setexcl(cgs->index[icg],cgs->index[icg+1],excl,TRUE,bexcl);
    
    naaj=calc_naaj(icg,cgs->nr);
    if (bTriclinic)
      ns_inner_tric(fr->cg_cm,icg,i_egp_flags,naaj,&(aaj[icg]),
		    box,b_inv,rlist2,cgs,ns_buf,md->cENER,
		    bHaveVdW,ngid,md,bexcl,fr);
    else
      ns_inner_rect(fr->cg_cm,icg,i_egp_flags,naaj,&(aaj[icg]),
		    bBox,box_size,b_inv,rlist2,cgs,ns_buf,md->cENER,
		    bHaveVdW,ngid,md,bexcl,fr);
    nsearch += naaj;
    
    for(nn=0; (nn<ngid); nn++) {
      for(k=0; (k<SHIFTS); k++) {
	nsbuf = &(ns_buf[nn][k]);
	if (nsbuf->ncg > 0) {
	  put_in_list(bHaveVdW,ngid,md,icg,nn,nsbuf->ncg,nsbuf->jcg,
		      cgs->index,/* cgs->a, */ bexcl,k,fr,FALSE,TRUE,TRUE);
	  nsbuf->ncg=nsbuf->nj=0;
	}
      }
    }
    /* setexcl(nri,i_atoms,excl,FALSE,bexcl); */
    setexcl(cgs->index[icg],cgs->index[icg+1],excl,FALSE,bexcl);
  }
  close_neighbor_list(fr,FALSE,-1,-1);
  
  return nsearch;
}

/************************************************
 *
 *    N S 5     G R I D     S T U F F
 *
 ************************************************/

static inline void get_dx(int Nx,real gridx,real grid_x,real rc2,real x,
			      int *dx0,int *dx1,real *dcx2)
{
  real dcx,tmp;
  int  xgi,xgi0,xgi1,i;

  xgi = (int)(Nx+x*grid_x)-Nx;
  if (xgi < 0) {
    *dx0 = 0;
    xgi0 = -1;
    *dx1 = -1;
    xgi1 = 0;
  } else if (xgi >= Nx) {
    *dx0 = Nx;
    xgi0 = Nx-1;
    *dx1 = Nx-1;
    xgi1 = Nx;
  } else {
    dcx2[xgi] = 0;
    *dx0 = xgi;
    xgi0 = xgi-1;
    *dx1 = xgi;
    xgi1 = xgi+1;
  }

  for(i=xgi0; i>=0; i--) {
     dcx = (i+1)*gridx-x;
     tmp = dcx*dcx;
     if (tmp >= rc2)
       break;
     *dx0 = i;
     dcx2[i] = tmp;
  }
  for(i=xgi1; i<Nx; i++) {
     dcx = i*gridx-x;
     tmp = dcx*dcx;
     if (tmp >= rc2)
       break;
     *dx1 = i;
     dcx2[i] = tmp;
  }
}

#define sqr(x) ((x)*(x))
#define calc_dx2(XI,YI,ZI,y) (sqr(XI-y[XX])+sqr(YI-y[YY])+sqr(ZI-y[ZZ]))
#define calc_cyl_dx2(XI,YI,y) (sqr(XI-y[XX])+sqr(YI-y[YY]))
/****************************************************
 *
 *    F A S T   N E I G H B O R  S E A R C H I N G
 *
 *    Optimized neighboursearching routine using grid 
 *    at least 1x1x1, see GROMACS manual
 *
 ****************************************************/

static void do_longrange(FILE *log,t_commrec *cr,t_topology *top,t_forcerec *fr,
			 int ngid,t_mdatoms *md,int icg,
			 int jgid,int nlr,
			 atom_id lr[],t_excl bexcl[],int shift,
			 rvec x[],rvec box_size,t_nrnb *nrnb,
			 real lambda,real *dvdlambda,
			 t_groups *grps,bool bDoVdW,bool bDoCoul,
			 bool bEvaluateNow,bool bHaveVdW[],bool bDoForces)
{
  int n,i;
  t_nblist *nl;

  for(n=0; n<fr->nnblists; n++) {
    for(i=0; (i<eNL_NR); i++) {
      nl = &fr->nblists[n].nlist_lr[i];
      if ((nl->nri > nl->maxnri-32) || bEvaluateNow) {
	close_neighbor_list(fr,TRUE,n,i);
	/* Evaluate the energies and forces */
	do_nonbonded(log,cr,fr,x,fr->f_twin,md,
		     grps->estat.ee[fr->bBHAM ? egBHAMLR : egLJLR],
		     grps->estat.ee[egCOULLR],box_size,
		     nrnb,lambda,dvdlambda,TRUE,n,i,bDoForces);
        
	reset_neighbor_list(fr,TRUE,n,i);
      }
    }
  }

  if (!bEvaluateNow) {  
    /* Put the long range particles in a list */
    put_in_list(bHaveVdW,ngid,md,icg,jgid,nlr,lr,top->blocks[ebCGS].index,
		/* top->blocks[ebCGS].a, */ bexcl,shift,fr,
		TRUE,bDoVdW,bDoCoul);
  }
}

static int ns5_core(FILE *log,t_commrec *cr,t_forcerec *fr,int cg_index[],
		    matrix box,rvec box_size,int ngid,
		    t_topology *top,t_groups *grps,
		    t_grid *grid,rvec x[],t_excl bexcl[],bool *bExcludeAlleg,
		    t_nrnb *nrnb,t_mdatoms *md,
		    real lambda,real *dvdlambda,
		    bool bHaveVdW[],bool bDoForces)
{
  static atom_id **nl_lr_ljc,**nl_lr_one,**nl_sr=NULL;
  static int     *nlr_ljc,*nlr_one,*nsr;
  static real *dcx2=NULL,*dcy2=NULL,*dcz2=NULL;
  
  t_block *cgs=&(top->blocks[ebCGS]);
  unsigned short  *gid=md->cENER;
  /* atom_id *i_atoms,*cgsindex=cgs->index; */
  int     tx,ty,tz,dx,dy,dz,cj;
#ifdef ALLOW_OFFDIAG_LT_HALFDIAG
  int zsh_ty,zsh_tx,ysh_tx;
#endif
  int     dx0,dx1,dy0,dy1,dz0,dz1;
  int     Nx,Ny,Nz,shift=-1,j,nrj,nns,nn=-1;
  real    gridx,gridy,gridz,grid_x,grid_y,grid_z;
  real    margin_x,margin_y;
  int     cg0,icg=-1,iicg,cgsnr,i0,nri,naaj,min_icg,icg_naaj,jjcg,cgj0,jgid;
  int     *grida,*gridnra,*gridind;
  bool    rvdw_lt_rcoul,rcoul_lt_rvdw;
  rvec    xi,*cgcm;
  real    r2,rs2,rvdw2,rcoul2,rm2,rl2,XI,YI,ZI,dcx,dcy,dcz,tmp1,tmp2;
  bool    *i_egp_flags;
  
  cgsnr    = cgs->nr;
  rs2      = sqr(fr->rlist);
  if (fr->bTwinRange) {
    rvdw2  = sqr(fr->rvdw);
    rcoul2 = sqr(fr->rcoulomb);
  } else {
    /* Workaround for a gcc -O3 or -ffast-math problem */
    rvdw2  = rs2;
    rcoul2 = rs2;
  }
  rm2 = min(rvdw2,rcoul2);
  rl2 = max(rvdw2,rcoul2);
  rvdw_lt_rcoul = (rvdw2 >= rcoul2);
  rcoul_lt_rvdw = (rcoul2 >= rvdw2);
  
  if (nl_sr == NULL) {
    /* Short range buffers */
    snew(nl_sr,ngid);
    /* Counters */
    snew(nsr,ngid);
    snew(nlr_ljc,ngid);
    snew(nlr_one,ngid);

    if (rm2 > rs2) 
      /* Long range VdW and Coul buffers */
      snew(nl_lr_ljc,ngid);
    
    if (rl2 > rm2)
      /* Long range VdW or Coul only buffers */
      snew(nl_lr_one,ngid);
    
    for(j=0; (j<ngid); j++) {
      snew(nl_sr[j],MAX_CG);
      if (rm2 > rs2)
	snew(nl_lr_ljc[j],MAX_CG);
      if (rl2 > rm2)
	snew(nl_lr_one[j],MAX_CG);
    }
    if (debug)
      fprintf(debug,"ns5_core: rs2 = %g, rvdw2 = %g, rcoul2 = %g (nm^2)\n",
	      rs2,rvdw2,rcoul2);
  }

  /* Unpack arrays */
  cgcm    = fr->cg_cm;
  Nx      = grid->nrx;
  Ny      = grid->nry;
  Nz      = grid->nrz;
  grida   = grid->a;
  gridind = grid->index;
  gridnra = grid->nra;
  nns     = 0;

  if (dcx2 == NULL) {
    /* Allocate tmp arrays */
    snew(dcx2,Nx*2);
    snew(dcy2,Ny*2);
    snew(dcz2,Nz*2);
  }

  gridx      = box[XX][XX]/grid->nrx;
  gridy      = box[YY][YY]/grid->nry;
  gridz      = box[ZZ][ZZ]/grid->nrz;
  grid_x     = 1/gridx;
  grid_y     = 1/gridy;
  grid_z     = 1/gridz;

#ifdef ALLOW_OFFDIAG_LT_HALFDIAG
  zsh_ty = floor(-box[ZZ][YY]/box[YY][YY]+0.5);
  zsh_tx = floor(-box[ZZ][XX]/box[XX][XX]+0.5);
  ysh_tx = floor(-box[YY][XX]/box[XX][XX]+0.5);
  if (zsh_tx!=0 && ysh_tx!=0)
    /* This could happen due to rounding, when both ratios are 0.5 */
    ysh_tx = 0;
#endif

  debug_gmx();

  if (fr->bTPI)
    /* We only want a list for the test particle */
    cg0 = cgsnr - 1;
  else
    cg0 = fr->cg0;

  /* Loop over charge groups */
  for(iicg=cg0; (iicg < fr->hcg); iicg++) {
    icg      = cg_index[iicg];
    /* Consistency check */
    if (icg != iicg)
      gmx_fatal(FARGS,"icg = %d, iicg = %d, file %s, line %d",icg,iicg,__FILE__,
		  __LINE__);

    /* Skip this charge group if all energy groups are excluded! */
    if(bExcludeAlleg[icg])
      continue;
    
    /*
    i0        = cgsindex[icg];
    nri       = cgsindex[icg+1]-i0;
    i_atoms   = &(cgsatoms[i0]);
    i_eg_excl = fr->eg_excl + ngid*gid[*i_atoms];
    */
    i_egp_flags = fr->egp_flags + ngid*gid[cgs->index[icg]];

    /* Set the exclusions for the atoms in charge group icg using
     * a bitmask
     */    
    /* setexcl(nri,i_atoms,&top->atoms.excl,TRUE,bexcl); */
    setexcl(cgs->index[icg],cgs->index[icg+1],&top->atoms.excl,TRUE,bexcl);
    
    /* Compute the number of charge groups that fall within the control
     * of this one (icg)
     */
    naaj     = calc_naaj(icg,cgsnr);
    icg_naaj = icg+naaj;
    if (fr->bTPI)
      /* The i-particle is awlways the test particle,
       * so we want all j-particles
       */
      min_icg = cgsnr - 1;
    else
      min_icg  = icg_naaj - cgsnr;

    /* Changed iicg to icg, DvdS 990115 
     * (but see consistency check above, DvdS 990330) 
     */
#ifdef NS5DB
    fprintf(log,"icg=%5d, naaj=%5d\n",icg,naaj);
#endif
    /* Loop over shift vectors in three dimensions */
    for (tz=-1; tz<=1; tz++) {
      ZI = cgcm[icg][ZZ]+tz*box[ZZ][ZZ];
      /* Calculate range of cells in Z direction that have the shift tz */
      get_dx(Nz,gridz,grid_z,rl2,ZI,&dz0,&dz1,dcz2);
      if (dz0 > dz1)
	continue;
#ifdef ALLOW_OFFDIAG_LT_HALFDIAG
      for (ty=-1+zsh_ty*tz; ty<=1+zsh_ty*tz; ty++) {
#else
      for (ty=-1; ty<=1; ty++) {
#endif 
	YI = cgcm[icg][YY]+ty*box[YY][YY]+tz*box[ZZ][YY];
	/* Calculate range of cells in Y direction that have the shift ty */
	get_dx(Ny,gridy,grid_y,rl2,YI,&dy0,&dy1,dcy2);
	if (dy0 > dy1)
	  continue;
#ifdef ALLOW_OFFDIAG_LT_HALFDIAG
	for (tx=-1+zsh_tx*tz+ysh_tx*ty; tx<=1+zsh_tx*tz+ysh_tx*ty; tx++) {
#else
        for (tx=-1; tx<=1; tx++) {
#endif
	  XI = cgcm[icg][XX]+tx*box[XX][XX]+ty*box[YY][XX]+tz*box[ZZ][XX];
	  /* Calculate range of cells in X direction that have the shift tx */
	  get_dx(Nx,gridx,grid_x,rl2,XI,&dx0,&dx1,dcx2);
	  if (dx0 > dx1)
	    continue;
	  /* Get shift vector */	  
	  shift=XYZ2IS(tx,ty,tz);
#ifdef NS5DB
	  range_check(shift,0,SHIFTS);
#endif
	  for(nn=0; (nn<ngid); nn++) {
	    nsr[nn]      = 0;
	    nlr_ljc[nn]  = 0;
	    nlr_one[nn] = 0;
	  }
#ifdef NS5DB
	  fprintf(log,"shift: %2d, dx0,1: %2d,%2d, dy0,1: %2d,%2d, dz0,1: %2d,%2d\n",
		  shift,dx0,dx1,dy0,dy1,dz0,dz1);
	  fprintf(log,"cgcm: %8.3f  %8.3f  %8.3f\n",cgcm[icg][XX],
		  cgcm[icg][YY],cgcm[icg][ZZ]);
	  fprintf(log,"xi:   %8.3f  %8.3f  %8.3f\n",XI,YI,ZI);
#endif
	  for (dx=dx0; (dx<=dx1); dx++) {
	    tmp1 = rl2 - dcx2[dx];
	    for (dy=dy0; (dy<=dy1); dy++) {
	      tmp2 = tmp1 - dcy2[dy];
	      if (tmp2 > 0)
		for (dz=dz0; (dz<=dz1); dz++) {
		  if (tmp2 > dcz2[dz]) {
		    /* Find grid-cell cj in which possible neighbours are */
		    cj   = xyz2ci(Ny,Nz,dx,dy,dz);
		    
		    /* Check out how many cgs (nrj) there in this cell */
		    nrj  = gridnra[cj];
		    
		    /* Find the offset in the cg list */
		    cgj0 = gridind[cj];
		    
		    /* Loop over cgs */
		    for (j=0; (j<nrj); j++) {
		      jjcg = grida[cgj0+j];
		      
		      /* check whether this guy is in range! */
		      if (((jjcg >= icg) && (jjcg < icg_naaj)) ||
			  ((jjcg < min_icg))) {
			r2=calc_dx2(XI,YI,ZI,cgcm[jjcg]);
			if (r2 < rl2) {
			  /* jgid = gid[cgsatoms[cgsindex[jjcg]]]; */
			  jgid = gid[cgs->index[jjcg]];
			  /* check energy group exclusions */
			  if (!(i_egp_flags[jgid] & EGP_EXCL)) {
			    if (r2 < rs2) {
			      if (nsr[jgid] >= MAX_CG) {
				put_in_list(bHaveVdW,ngid,md,icg,jgid,
					    nsr[jgid],nl_sr[jgid],
					    cgs->index,/* cgsatoms, */ bexcl,
					    shift,fr,FALSE,TRUE,TRUE);
				nsr[jgid]=0;
			      }
			      nl_sr[jgid][nsr[jgid]++]=jjcg;
			    } else if (r2 < rm2) {
			      if (nlr_ljc[jgid] >= MAX_CG) {
				do_longrange(log,cr,top,fr,ngid,md,icg,jgid,
					     nlr_ljc[jgid],
					     nl_lr_ljc[jgid],bexcl,shift,x,
					     box_size,nrnb,
					     lambda,dvdlambda,grps,
					     TRUE,TRUE,FALSE,
					     bHaveVdW,bDoForces);
				nlr_ljc[jgid]=0;
			      }
			      nl_lr_ljc[jgid][nlr_ljc[jgid]++]=jjcg;
			    } else {
			      if (nlr_one[jgid] >= MAX_CG) {
				do_longrange(log,cr,top,fr,ngid,md,icg,jgid,
					     nlr_one[jgid],
					     nl_lr_one[jgid],bexcl,shift,x,
					     box_size,nrnb,
					     lambda,dvdlambda,grps,
					     rvdw_lt_rcoul,rcoul_lt_rvdw,FALSE,
					     bHaveVdW,bDoForces);
				nlr_one[jgid]=0;
			      }
			      nl_lr_one[jgid][nlr_one[jgid]++]=jjcg;
			    }
			  }
			}
			nns++;
		      }
		    }
		  }
		}
	    }
	  }
	  /* CHECK whether there is anything left in the buffers */
	  for(nn=0; (nn<ngid); nn++) {
	    if (nsr[nn] > 0)
	      put_in_list(bHaveVdW,ngid,md,icg,nn,nsr[nn],nl_sr[nn],
			  cgs->index, /* cgsatoms, */ bexcl,
			  shift,fr,FALSE,TRUE,TRUE);
	    
	    if (nlr_ljc[nn] > 0) 
	      do_longrange(log,cr,top,fr,ngid,md,icg,nn,nlr_ljc[nn],
			   nl_lr_ljc[nn],bexcl,shift,x,box_size,nrnb,
			   lambda,dvdlambda,grps,TRUE,TRUE,FALSE,
			   bHaveVdW,bDoForces);
	    
	    if (nlr_one[nn] > 0) 
	      do_longrange(log,cr,top,fr,ngid,md,icg,nn,nlr_one[nn],
			   nl_lr_one[nn],bexcl,shift,x,box_size,nrnb,
			   lambda,dvdlambda,grps,
			   rvdw_lt_rcoul,rcoul_lt_rvdw,FALSE,
			   bHaveVdW,bDoForces);
	  }
	}
      }
    }
    /* setexcl(nri,i_atoms,&top->atoms.excl,FALSE,bexcl); */
    setexcl(cgs->index[icg],cgs->index[icg+1],&top->atoms.excl,FALSE,bexcl);
  }
  /* Perform any left over force calculations */
  for (nn=0; (nn<ngid); nn++) {
    if (rm2 > rs2)
      do_longrange(log,cr,top,fr,0,md,icg,nn,nlr_ljc[nn],
		   nl_lr_ljc[nn],bexcl,shift,x,box_size,nrnb,
		   lambda,dvdlambda,grps,TRUE,TRUE,TRUE,bHaveVdW,bDoForces);
    if (rl2 > rm2)
      do_longrange(log,cr,top,fr,0,md,icg,nn,nlr_one[nn],
		   nl_lr_one[nn],bexcl,shift,x,box_size,nrnb,
		   lambda,dvdlambda,grps,rvdw_lt_rcoul,rcoul_lt_rvdw,
		   TRUE,bHaveVdW,bDoForces);
  }
  debug_gmx();
  
  /* Close off short range neighbourlists */
  close_neighbor_list(fr,FALSE,-1,-1);
  return nns;
}

static rvec *sptr;
static int  sdim;

static int  rv_comp(const void *a,const void *b)
{
  int  ia = *(int *)a;
  int  ib = *(int *)b;
  real diff;
  
  diff = sptr[ia][sdim] - sptr[ib][sdim];
  if (diff < 0)
    return -1;
  else if (diff == 0)
    return 0;
  else
    return 1;
}

static void sort_charge_groups(t_commrec *cr,int cg_index[],int slab_index[],
			       rvec cg_cm[],int Dimension)
{
  int i,nrcg,cgind;
  
  nrcg = slab_index[cr->nnodes];
  sptr = cg_cm;
  sdim = Dimension;
  qsort(cg_index,nrcg,sizeof(cg_index[0]),rv_comp);
  
  if (debug) {
    fprintf(debug,"Just sorted the cg_cm array on dimension %d\n",Dimension);
    fprintf(debug,"Index:  Coordinates of cg_cm\n");
    for(i=0; (i<nrcg); i++) {
      cgind = cg_index[i];
      fprintf(debug,"%8d%10.3f%10.3f%10.3f\n",cgind,
	      cg_cm[cgind][XX],cg_cm[cgind][YY],cg_cm[cgind][ZZ]);
    }
  }
  sptr = NULL;
  sdim = -1;
}

int search_neighbours(FILE *log,t_forcerec *fr,
		      rvec x[],matrix box,
		      t_topology *top,t_groups *grps,
		      t_commrec *cr,t_nsborder *nsb,
		      t_nrnb *nrnb,t_mdatoms *md,
		      real lambda,real *dvdlambda,
		      bool bFillGrid,bool bDoForces)
{
  static   bool        bFirst=TRUE;
  static   t_grid      *grid=NULL;
  static   t_excl      *bexcl;
  static   bool        *bHaveVdW;
  static   t_ns_buf    **ns_buf=NULL;
  static   int         *cg_index=NULL,*slab_index=NULL;
  static   bool        *bExcludeAlleg;
  
  t_block  *cgs=&(top->blocks[ebCGS]);
  rvec     box_size;
  int      i,j,m,ngid;
  real     min_size;
  bool     allexcl;
  int      nsearch;
  bool     bGrid;
  char     *ptr;
  bool     *i_egp_flags;
  int      start,end;
  
  /* Set some local variables */
  bGrid=fr->bGrid;
  ngid=top->atoms.grps[egcENER].nr;
  
  for(m=0; (m<DIM); m++)
    box_size[m]=box[m][m];
  
  if (fr->ePBC != epbcNONE) {
    if (bGrid) {
      if (sqr(fr->rlistlong) >= max_cutoff2(box))
	gmx_fatal(FARGS,"One of the box vectors has become shorter than twice the cut-off length or one of the box diagonal elements has become smaller than the cut-off.");
    } else {
      min_size = min(box_size[XX],min(box_size[YY],box_size[ZZ]));
      if (2*fr->rlistlong >= min_size)
	gmx_fatal(FARGS,"One of the box lengths has become smaller than twice the cut-off length.");
    }
  }

  /* First time initiation of arrays etc. */  
  if (bFirst) {
    int icg,nr_in_cg,maxcg;
    
    /* Compute largest charge groups size (# atoms) */
    nr_in_cg=1;
    for (icg=0; (icg < cgs->nr); icg++)
      nr_in_cg=max(nr_in_cg,(int)(cgs->index[icg+1]-cgs->index[icg]));

    /* Verify whether largest charge group is <= max cg.
     * This is determined by the type of the local exclusion type 
     * Exclusions are stored in bits. (If the type is not large
     * enough, enlarge it, unsigned char -> unsigned short -> unsigned long)
     */
    maxcg=sizeof(t_excl)*8;
    if (nr_in_cg > maxcg)
      gmx_fatal(FARGS,"Max #atoms in a charge group: %d > %d\n",
		  nr_in_cg,maxcg);
      
    snew(bexcl,cgs->nra);

    /* Check for charge groups with all energy groups excluded */
    snew(bExcludeAlleg,cgs->nr);
    for(i=0;i<cgs->nr;i++) {
      allexcl=TRUE;
      /* Make ptr to the excl list for the 1st atoms energy group */
      /* i_egp_flags = fr->egp_flags + ngid*md->cENER[cgs->a[cgs->index[i]]]; */
      i_egp_flags = fr->egp_flags + ngid*md->cENER[cgs->index[i]];
      
      for(j=0; j<ngid && allexcl;j++) {
	if(!(i_egp_flags[j] & EGP_EXCL)) {
	  /* Not excluded, i.e. we should search neighbors */ 
	  allexcl=FALSE;
	}
      }
      bExcludeAlleg[i]=allexcl;
    }
    
    debug_gmx();

    if ((ptr=getenv("NLIST")) != NULL) {
      sscanf(ptr,"%d",&NLJ_INC);
      
      fprintf(log,"%s: I will increment J-lists by %d\n",
	      __FILE__,NLJ_INC);
    }

    /* Check whether we have to do domain decomposition,
     * if so set local variables for the charge group index and the
     * slab index.
     */
    if (fr->bDomDecomp) {
      snew(slab_index,cr->nnodes+1);
      for(i=0; (i<=cr->nnodes); i++)
	slab_index[i] = i*((real) cgs->nr/((real) cr->nnodes));
      fr->cg0 = slab_index[cr->nodeid];
      fr->hcg = slab_index[cr->nodeid+1] - fr->cg0;
      if (debug)
	fprintf(debug,"Will use DOMAIN DECOMPOSITION, "
		"from charge group index %d to %d on node %d\n",
		fr->cg0,fr->cg0+fr->hcg,cr->nodeid);
    }
    snew(cg_index,cgs->nr+1);
    for(i=0; (i<=cgs->nr);  i++)
      cg_index[i] = i;
    
    if (bGrid) {
      snew(grid,1);
      init_grid(log,grid,fr->ndelta,box,fr->rlistlong,cgs->nr);
    }
    
    /* Create array that determines whether or not atoms have VdW */
    snew(bHaveVdW,fr->ntype);
    for(i=0; (i<fr->ntype); i++) {
      for(j=0; (j<fr->ntype); j++) {
	bHaveVdW[i] = (bHaveVdW[i] || 
		      (fr->bBHAM ? 
		       ((BHAMA(fr->nbfp,fr->ntype,i,j) != 0) ||
			(BHAMB(fr->nbfp,fr->ntype,i,j) != 0) ||
			(BHAMC(fr->nbfp,fr->ntype,i,j) != 0)) :
		       ((C6(fr->nbfp,fr->ntype,i,j) != 0) ||
			(C12(fr->nbfp,fr->ntype,i,j) != 0))));
      }
    }
    if (debug) 
      pr_ivec(debug,0,"bHaveVdW",bHaveVdW,fr->ntype,TRUE);
    
    bFirst=FALSE;
  }
  debug_gmx();
  
  /* Reset the neighbourlists */
  reset_neighbor_list(fr,FALSE,-1,-1);

  if (bGrid && bFillGrid) {
    grid_first(log,grid,box,fr->rlistlong);
    debug_gmx();

    /* Don't know why this all is... (DvdS 3/99) */
#ifndef SEGV
    start = 0;
    end   = cgs->nr;
#else
    start = fr->cg0;
    end   = (cgs->nr+1)/2;
#endif

    if (fr->bDomDecomp)
      sort_charge_groups(cr,cg_index,slab_index,fr->cg_cm,fr->Dimension);
    debug_gmx();
    
    fill_grid(log,fr->bDomDecomp,cg_index,
	      grid,box,cgs->nr,fr->cg0,fr->hcg,fr->cg_cm);
    debug_gmx();

    if (PAR(cr))
      mv_grid(cr,fr->bDomDecomp,cg_index,grid,nsb->workload);
    debug_gmx();
      
    calc_elemnr(log,fr->bDomDecomp,cg_index,grid,start,end,cgs->nr);
    calc_ptrs(grid);
    grid_last(log,fr->bDomDecomp,cg_index,grid,start,end,cgs->nr);

    if (debug) {
      check_grid(debug,grid);
      print_grid(debug,grid,fr->bDomDecomp,cg_index);
    }
  }
  debug_gmx();
  
  /* Do the core! */
  if (bGrid)
    nsearch = ns5_core(log,cr,fr,cg_index,box,box_size,ngid,top,grps,
		       grid,x,bexcl,bExcludeAlleg,nrnb,md,lambda,dvdlambda,
		       bHaveVdW,bDoForces);
  else {
    /* Only allocate this when necessary, saves 100 kb */
    if (ns_buf == NULL) {
      snew(ns_buf,ngid);
      for(i=0; (i<ngid); i++)
	snew(ns_buf[i],SHIFTS);
    }
    nsearch = ns_simple_core(fr,top,md,box,box_size,
			     bexcl,ngid,ns_buf,bHaveVdW);
  }
  debug_gmx();
  
#ifdef DEBUG
  pr_nsblock(log);
#endif

  inc_nrnb(nrnb,eNR_NS,nsearch);
  /* inc_nrnb(nrnb,eNR_LR,fr->nlr); */

  return nsearch;
}


