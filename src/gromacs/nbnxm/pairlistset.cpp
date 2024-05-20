/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright 2012- The GROMACS Authors
 * and the project initiators Erik Lindahl, Berk Hess and David van der Spoel.
 * Consult the AUTHORS/COPYING files and https://www.gromacs.org for details.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * https://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at https://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out https://www.gromacs.org.
 */

/*! \internal \file
 * \brief
 * Implements functionality for PairlistSet.
 *
 * \author Berk Hess <hess@kth.se>
 * \ingroup module_nbnxm
 */

#include "gmxpre.h"

#include "pairlistset.h"

#include "gromacs/mdtypes/nblist.h"
#include "gromacs/nbnxm/nbnxm.h"
#include "gromacs/pbcutil/ishift.h"
#include "gromacs/utility/gmxassert.h"

#include "pairlist.h"
#include "pairlistparams.h"
#include "pairlistwork.h"

PairlistSet::~PairlistSet() = default;

namespace
{

void appendPlainPairlistCpu(PlainPairlist*           plainPairlist,
                            const NbnxnPairlistCpu&  pairlist,
                            const PairlistParams&    params,
                            gmx::ArrayRef<const int> atomIndices)
{
    constexpr int                     c_maxClusterSize = 8;
    std::array<int, c_maxClusterSize> atomI;

    gmx::ArrayRef<const nbnxn_ci_t> ciList;
    gmx::ArrayRef<const nbnxn_cj_t> cjList;
    if (params.useDynamicPruning)
    {
        ciList = pairlist.ciOuter;
        cjList = pairlist.cjOuter;
    }
    else
    {
        GMX_ASSERT(pairlist.ciOuter.empty(), "With dynamic pruning we expect an empty outer i-list");
        GMX_ASSERT(pairlist.cjOuter.empty(), "With dynamic pruning we expect an empty outer j-list");
        ciList = pairlist.ci;
        cjList = pairlist.cj.list_;
    }

    for (const nbnxn_ci_t& iEntry : ciList)
    {
        const int shiftIndex = (iEntry.shift & NBNXN_CI_SHIFT);

        // Prefetch the i-atom indices
        for (int i = 0; i < pairlist.na_ci; i++)
        {
            atomI[i] = atomIndices[iEntry.ci * pairlist.na_ci + i];
        }

        for (int jClusterIndex = iEntry.cj_ind_start; jClusterIndex < iEntry.cj_ind_end; jClusterIndex++)
        {
            const nbnxn_cj_t& jEntry = cjList[jClusterIndex];

            for (int i = 0; i < pairlist.na_ci; i++)
            {
                if (atomI[i] < 0)
                {
                    // This is a filler particle
                    continue;
                }

                const int iAtomIndex = iEntry.ci * pairlist.na_ci + i;

                for (int j = 0; j < pairlist.na_cj; j++)
                {
                    const int jAtomIndex = jEntry.cj * pairlist.na_cj + j;
                    const int atomJ      = atomIndices[jAtomIndex];

                    if (atomJ >= 0)
                    {
                        if (jEntry.excl & (1 << (i * pairlist.na_cj + j)))
                        {
                            plainPairlist->pairs.push_back({ { atomI[i], atomJ }, shiftIndex });
                        }
                        else if (shiftIndex != gmx::c_centralShiftIndex || jAtomIndex > iAtomIndex)
                        {
                            GMX_ASSERT(atomJ != atomI[i], "We should not add self-pairs");

                            plainPairlist->excludedPairs.push_back({ { atomI[i], atomJ }, shiftIndex });
                        }
                    }
                }
            }
        }
    }
}

void appendPlainPairlistGpu(PlainPairlist*           plainPairlist,
                            const NbnxnPairlistGpu&  pairlist,
                            gmx::ArrayRef<const int> atomIndices)
{
    constexpr int c_clSize           = c_nbnxnGpuClusterSize;
    constexpr int c_splitClusterSize = c_nbnxnGpuClusterSize / c_nbnxnGpuClusterpairSplit;

    GMX_ASSERT(pairlist.na_ci == c_clSize,
               "The cluster size in the pairlist should match the GPU cluster size");
    GMX_ASSERT(pairlist.na_cj == c_clSize,
               "The cluster size in the pairlist should match the GPU cluster size");

    constexpr unsigned int c_superClInteractionMask = ((1U << c_nbnxnGpuNumClusterPerSupercluster) - 1U);

    std::array<int, c_nbnxnGpuNumClusterPerSupercluster * c_clSize> atomIList;

    for (const nbnxn_sci& iEntry : pairlist.sci)
    {
        // Prefetch the i-atom indices
        for (int i = 0; i < c_nbnxnGpuNumClusterPerSupercluster * c_clSize; i++)
        {
            atomIList[i] = atomIndices[iEntry.sci * c_nbnxnGpuNumClusterPerSupercluster * c_clSize + i];
        }

        for (int jPackIndex = iEntry.cjPackedBegin; jPackIndex < iEntry.cjPackedEnd; jPackIndex++)
        {
            const nbnxn_cj_packed_t& jPack = pairlist.cjPacked.list_[jPackIndex];

            for (int jInPack = 0; jInPack < c_nbnxnGpuJgroupSize; jInPack++)
            {
                GMX_ASSERT(jPack.imei[0].imask == jPack.imei[1].imask,
                           "This code assumed that the pairlist is not pruned and contains whole "
                           "cluster pairs");

                if ((jPack.imei[0].imask
                     & (c_superClInteractionMask << (jInPack * c_nbnxnGpuNumClusterPerSupercluster)))
                    == 0)
                {
                    continue;
                }

                const int jCluster = jPack.cj[jInPack];

                for (int iClusterIndex = 0; iClusterIndex < c_gpuNumClusterPerCell; iClusterIndex++)
                {
                    const unsigned int clusterPairMask =
                            (1U << (jInPack * c_nbnxnGpuNumClusterPerSupercluster + iClusterIndex));
                    if ((jPack.imei[0].imask & clusterPairMask) == 0)
                    {
                        continue;
                    }

                    for (int i = 0; i < c_clSize; i++)
                    {
                        const int atomI = atomIList[iClusterIndex * c_clSize + i];

                        if (atomI < 0)
                        {
                            // This is a filler particle
                            continue;
                        }

                        const int iAtomIndex =
                                (iEntry.sci * c_nbnxnGpuNumClusterPerSupercluster + iClusterIndex) * c_clSize
                                + i;

                        for (int j = 0; j < c_clSize; j++)
                        {
                            const int jAtomIndex = jCluster * c_clSize + j;
                            const int atomJ      = atomIndices[jAtomIndex];

                            const int exclPair = atomIndexInClusterpairSplit(j) * c_clSize + i;

                            if (atomJ >= 0)
                            {
                                if (pairlist.excl[jPack.imei[j / c_splitClusterSize].excl_ind].pair[exclPair]
                                    & clusterPairMask)
                                {
                                    plainPairlist->pairs.push_back({ { atomI, atomJ }, iEntry.shift });
                                }
                                else if (iEntry.shift != gmx::c_centralShiftIndex || jAtomIndex > iAtomIndex)
                                {
                                    plainPairlist->excludedPairs.push_back(
                                            { { atomI, atomJ }, iEntry.shift });
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

} // namespace

void PairlistSet::appendPlainPairlist(PlainPairlist* plainPairlist, gmx::ArrayRef<const int> atomIndices)
{
    if (sc_isGpuPairListType[params_.pairlistType])
    {
        GMX_ASSERT(gpuList() != nullptr, "We expect a GPU pairlist to be present");

        appendPlainPairlistGpu(plainPairlist, *gpuList(), atomIndices);
    }
    else
    {
        for (const auto& cpuList : cpuLists())
        {
            appendPlainPairlistCpu(plainPairlist, cpuList, params_, atomIndices);
        }
    }
}
