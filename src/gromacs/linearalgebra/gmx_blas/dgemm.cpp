#include <cctype>
#include <cmath>

#include "../gmx_blas.h"

#include "gromacs/utility/real.h"

void F77_FUNC(dgemm, DGEMM)(const char* transa,
                            const char* transb,
                            int*        m__,
                            int*        n__,
                            int*        k__,
                            double*     alpha__,
                            double*     a,
                            int*        lda__,
                            double*     b,
                            int*        ldb__,
                            double*     beta__,
                            double*     c,
                            int*        ldc__)
{
    const char tra = std::toupper(*transa);
    const char trb = std::toupper(*transb);
    double     temp;
    int        i, j, l;

    int m   = *m__;
    int n   = *n__;
    int k   = *k__;
    int lda = *lda__;
    int ldb = *ldb__;
    int ldc = *ldc__;

    double alpha = *alpha__;
    double beta  = *beta__;

    if (m == 0 || n == 0
        || ((std::abs(alpha) < GMX_DOUBLE_MIN || k == 0) && std::abs(beta - 1.0) < GMX_DOUBLE_EPS))
        return;

    if (std::abs(alpha) < GMX_DOUBLE_MIN)
    {
        if (std::abs(beta) < GMX_DOUBLE_MIN)
        {
            for (j = 0; j < n; j++)
                for (i = 0; i < m; i++)
                    c[j * (ldc) + i] = 0.0;
        }
        else
        {
            /* nonzero beta */
            for (j = 0; j < n; j++)
                for (i = 0; i < m; i++)
                    c[j * (ldc) + i] *= beta;
        }
        return;
    }

    if (trb == 'N')
    {
        if (tra == 'N')
        {

            for (j = 0; j < n; j++)
            {
                if (std::abs(beta) < GMX_DOUBLE_MIN)
                {
                    for (i = 0; i < m; i++)
                        c[j * (ldc) + i] = 0.0;
                }
                else if (std::abs(beta - 1.0) > GMX_DOUBLE_EPS)
                {
                    for (i = 0; i < m; i++)
                        c[j * (ldc) + i] *= beta;
                }
                for (l = 0; l < k; l++)
                {
                    if (std::abs(b[j * (ldb) + l]) > GMX_DOUBLE_MIN)
                    {
                        temp = alpha * b[j * (ldb) + l];
                        for (i = 0; i < m; i++)
                            c[j * (ldc) + i] += temp * a[l * (lda) + i];
                    }
                }
            }
        }
        else
        {
            /* transpose A, but not B */
            for (j = 0; j < n; j++)
            {
                for (i = 0; i < m; i++)
                {
                    temp = 0.0;
                    for (l = 0; l < k; l++)
                        temp += a[i * (lda) + l] * b[j * (ldb) + l];
                    if (std::abs(beta) < GMX_DOUBLE_MIN)
                        c[j * (ldc) + i] = alpha * temp;
                    else
                        c[j * (ldc) + i] = alpha * temp + beta * c[j * (ldc) + i];
                }
            }
        }
    }
    else
    {
        /* transpose B */
        if (tra == 'N')
        {

            /* transpose B, but not A */

            for (j = 0; j < n; j++)
            {
                if (std::abs(beta) < GMX_DOUBLE_MIN)
                {
                    for (i = 0; i < m; i++)
                        c[j * (ldc) + i] = 0.0;
                }
                else if (std::abs(beta - 1.0) > GMX_DOUBLE_EPS)
                {
                    for (i = 0; i < m; i++)
                        c[j * (ldc) + i] *= beta;
                }
                for (l = 0; l < k; l++)
                {
                    if (std::abs(b[l * (ldb) + j]) > GMX_DOUBLE_MIN)
                    {
                        temp = alpha * b[l * (ldb) + j];
                        for (i = 0; i < m; i++)
                            c[j * (ldc) + i] += temp * a[l * (lda) + i];
                    }
                }
            }
        }
        else
        {
            /* Transpose both A and B */
            for (j = 0; j < n; j++)
            {
                for (i = 0; i < m; i++)
                {
                    temp = 0.0;
                    for (l = 0; l < k; l++)
                        temp += a[i * (lda) + l] * b[l * (ldb) + j];
                    if (std::abs(beta) < GMX_DOUBLE_MIN)
                        c[j * (ldc) + i] = alpha * temp;
                    else
                        c[j * (ldc) + i] = alpha * temp + beta * c[j * (ldc) + i];
                }
            }
        }
    }
}
