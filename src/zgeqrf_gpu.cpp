/*
    -- MAGMA (version 1.0) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       November 2010

       @precisions normal z -> s d c

*/

#include <stdio.h>
#include <cuda_runtime_api.h>
#include <cublas.h>
#include "magma.h"

extern "C" magma_int_t
magma_zgeqrf_gpu(magma_int_t m, magma_int_t n,
                 cuDoubleComplex *a,   magma_int_t lda,
		 cuDoubleComplex *tau, magma_int_t *info)
{
/*  -- MAGMA (version 1.0) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       November 2010

    Purpose
    =======

    ZGEQRF computes a QR factorization of a real M-by-N matrix A:
    A = Q * R.

    Arguments
    =========

    M       (input) INTEGER
            The number of rows of the matrix A.  M >= 0.

    N       (input) INTEGER
            The number of columns of the matrix A.  N >= 0.

    A       (input/output) COMPLEX_16 array on the GPU, dimension (LDA,N)
            On entry, the M-by-N matrix A.
            On exit, the elements on and above the diagonal of the array
            contain the min(M,N)-by-N upper trapezoidal matrix R (R is
            upper triangular if m >= n); the elements below the diagonal,
            with the array TAU, represent the orthogonal matrix Q as a
            product of min(m,n) elementary reflectors (see Further
            Details).

    LDA     (input) INTEGER
            The leading dimension of the array A.  LDA >= max(1,M).
            To benefit from coalescent memory accesses LDA must be
            dividable by 16.

    TAU     (output) COMPLEX_16 array, dimension (min(M,N))
            The scalar factors of the elementary reflectors (see Further
            Details).

    INFO    (output) INTEGER
            = 0:  successful exit
            < 0:  if INFO = -i, the i-th argument had an illegal value
                  if INFO = -9, internal GPU memory allocation failed.

    Further Details
    ===============

    The matrix Q is represented as a product of elementary reflectors

       Q = H(1) H(2) . . . H(k), where k = min(m,n).

    Each H(i) has the form

       H(i) = I - tau * v * v'

    where tau is a real scalar, and v is a real vector with
    v(1:i-1) = 0 and v(i) = 1; v(i+1:m) is stored on exit in A(i+1:m,i),
    and tau in TAU(i).

    =====================================================================    */

   #define a_ref(a_1,a_2) ( a+(a_2)*(lda) + (a_1))
   #define work_ref(a_1)  ( work + (a_1))
   #define hwork          ( work + (nb)*(m))
   #define min(a,b)       (((a)<(b))?(a):(b))
   #define max(a,b)       (((a)>(b))?(a):(b))

    int i, k, ldwork, lddwork, old_i, old_ib, rows;
    int nbmin, nx, ib;

    /* Function Body */
    *info = 0;
    int nb = magma_get_zgeqrf_nb(m);

    if (m < 0) {
        *info = -1;
    } else if (n < 0) {
        *info = -2;
    } else if (lda < max(1,m)) {
        *info = -4;
    }
    if (*info != 0)
        return 0;

    k = min(m,n);
    if (k == 0)
        return 0;

    int lwork  = (m+n) * nb;
    int lhwork = lwork - (m)*nb;

    static cudaStream_t stream[2];
    cudaStreamCreate(&stream[0]);
    cudaStreamCreate(&stream[1]);

    cuDoubleComplex *dwork;
    cublasStatus status;
    status = cublasAlloc((n)*nb, sizeof(cuDoubleComplex), (void**)&dwork);
    if (status != CUBLAS_STATUS_SUCCESS) {
        *info = -9;
        return 0;
    }

    cuDoubleComplex *work;
    cudaMallocHost((void**)&work, lwork*sizeof(cuDoubleComplex));

    nbmin = 2;
    nx    = nb;
    ldwork = m;
    lddwork= n;

    if (nb >= nbmin && nb < k && nx < k) {
        /* Use blocked code initially */
        old_i = 0; old_ib = nb;
        for (i = 0; i < k-nx; i += nb) {
            ib = min(k-i, nb);
            rows = m -i;
            cudaMemcpy2DAsync( work_ref(i), ldwork*sizeof(cuDoubleComplex),
                               a_ref(i,i),  lda   *sizeof(cuDoubleComplex),
                               sizeof(cuDoubleComplex)*rows, ib,
                               cudaMemcpyDeviceToHost, stream[1]);
            if (i>0){
                /* Apply H' to A(i:m,i+2*ib:n) from the left */
                magma_zlarfb( MagmaLeft, MagmaConjTrans, MagmaForward, MagmaColumnwise,
                              m-old_i, n-old_i-2*old_ib, old_ib,
                              a_ref(old_i, old_i         ), lda, dwork,        lddwork,
                              a_ref(old_i, old_i+2*old_ib), lda, dwork+old_ib, lddwork);

                cudaMemcpy2DAsync( a_ref(old_i, old_i), lda   *sizeof(cuDoubleComplex),
                                   work_ref(old_i),     ldwork*sizeof(cuDoubleComplex),
                                   sizeof(cuDoubleComplex)*old_ib, old_ib,
                                   cudaMemcpyHostToDevice, stream[0]);
            }

            cudaStreamSynchronize(stream[1]);
            lapackf77_zgeqrf(&rows, &ib, work_ref(i), &ldwork, tau+i, hwork, &lhwork, info);
            /* Form the triangular factor of the block reflector
               H = H(i) H(i+1) . . . H(i+ib-1) */
            lapackf77_zlarft( MagmaForwardStr, MagmaColumnwiseStr, 
                              &rows, &ib, 
                              work_ref(i), &ldwork, tau+i, hwork, &ib);

            zpanel_to_q( MagmaUpper, ib, work_ref(i), ldwork, hwork+ib*ib );
            cublasSetMatrix(rows, ib, sizeof(cuDoubleComplex),
                            work_ref(i), ldwork, 
                            a_ref(i,i),  lda);
            zq_to_panel( MagmaUpper, ib, work_ref(i), ldwork, hwork+ib*ib );

            if (i + ib < n) {
                cublasSetMatrix(ib, ib, sizeof(cuDoubleComplex), 
                                hwork, ib, 
                                dwork, lddwork);

                if (i+nb < k-nx)
                    /* Apply H' to A(i:m,i+ib:i+2*ib) from the left */
                    magma_zlarfb( MagmaLeft, MagmaConjTrans, MagmaForward, MagmaColumnwise,
                                  rows, ib, ib, 
                                  a_ref(i, i   ), lda, dwork,    lddwork, 
                                  a_ref(i, i+ib), lda, dwork+ib, lddwork);
                else {
                    magma_zlarfb( MagmaLeft, MagmaConjTrans, MagmaForward, MagmaColumnwise,
                                  rows, n-i-ib, ib, 
                                  a_ref(i, i   ), lda, dwork,    lddwork, 
                                  a_ref(i, i+ib), lda, dwork+ib, lddwork);
                    cublasSetMatrix(ib, ib, sizeof(cuDoubleComplex),
                                    work_ref(i), ldwork,
                                    a_ref(i,i),  lda);
                }
                old_i  = i;
                old_ib = ib;
            }
        }
    } else {
        i = 0;
    }

    cublasFree(dwork);

    /* Use unblocked code to factor the last or only block. */
    if (i < k) {
        ib   = n-i;
        rows = m-i;
        cublasGetMatrix(rows, ib, sizeof(cuDoubleComplex),
                        a_ref(i, i), lda, 
                        work,        rows);
        lhwork = lwork - rows*ib;
        lapackf77_zgeqrf(&rows, &ib, work, &rows, tau+i, work+ib*rows, &lhwork, info);
        
        cublasSetMatrix(rows, ib, sizeof(cuDoubleComplex),
                        work,        rows, 
                        a_ref(i, i), lda);
    }
    cublasFree(work);

    return 0;

    /* End of MAGMA_ZGEQRF */

} /* magma_zgeqrf_ */
