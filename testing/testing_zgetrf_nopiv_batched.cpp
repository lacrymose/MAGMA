/*
   -- MAGMA (version 1.5) --
   Univ. of Tennessee, Knoxville
   Univ. of California, Berkeley
   Univ. of Colorado, Denver
   @date

   @author Azzam Haidar
   @author Tingxing Dong

   @precisions normal z -> s d c
 */
// includes, system
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cuda_runtime_api.h>
#include <cublas_v2.h>


// includes, project
#include "flops.h"
#include "magma.h"
#include "magma_lapack.h"
#include "testings.h"
#include "common_magma.h"

double get_LU_error(magma_int_t M, magma_int_t N,
                    magmaDoubleComplex *A,  magma_int_t lda,
                    magmaDoubleComplex *LU, magma_int_t *IPIV)
{
    magma_int_t min_mn = min(M,N);
    magma_int_t ione   = 1;
    magma_int_t i, j;
    magmaDoubleComplex alpha = MAGMA_Z_ONE;
    magmaDoubleComplex beta  = MAGMA_Z_ZERO;
    magmaDoubleComplex *L, *U;
    double work[1], matnorm, residual;
    
    TESTING_MALLOC_CPU( L, magmaDoubleComplex, M*min_mn);
    TESTING_MALLOC_CPU( U, magmaDoubleComplex, min_mn*N);
    memset( L, 0, M*min_mn*sizeof(magmaDoubleComplex) );
    memset( U, 0, min_mn*N*sizeof(magmaDoubleComplex) );

    lapackf77_zlaswp( &N, A, &lda, &ione, &min_mn, IPIV, &ione);
    lapackf77_zlacpy( MagmaLowerStr, &M, &min_mn, LU, &lda, L, &M      );
    lapackf77_zlacpy( MagmaUpperStr, &min_mn, &N, LU, &lda, U, &min_mn );

    for(j=0; j<min_mn; j++)
        L[j+j*M] = MAGMA_Z_MAKE( 1., 0. );
    
    matnorm = lapackf77_zlange("f", &M, &N, A, &lda, work);

    blasf77_zgemm("N", "N", &M, &N, &min_mn,
                  &alpha, L, &M, U, &min_mn, &beta, LU, &lda);

    for( j = 0; j < N; j++ ) {
        for( i = 0; i < M; i++ ) {
            LU[i+j*lda] = MAGMA_Z_SUB( LU[i+j*lda], A[i+j*lda] );
        }
    }
    residual = lapackf77_zlange("f", &M, &N, LU, &lda, work);

    TESTING_FREE_CPU(L);
    TESTING_FREE_CPU(U);

    return residual / (matnorm * N);
}

/* ////////////////////////////////////////////////////////////////////////////
   -- Testing zgetrf_batched
*/
int main( int argc, char** argv)
{
    TESTING_INIT();

    real_Double_t   gflops, magma_perf, magma_time, cublas_perf, cublas_time, cpu_perf=0, cpu_time=0;
    double          error; 
    magma_int_t cublas_enable = 0;
    magmaDoubleComplex *h_A, *h_R;
    magmaDoubleComplex *dA_magma, *dA_cublas;
    magmaDoubleComplex **dA_array = NULL;

    magma_int_t     **dipiv_array = NULL;
    magma_int_t     *ipiv;
    magma_int_t     *dipiv_magma, *dinfo_magma;
    magma_int_t     *dipiv_cublas, *dinfo_cublas;
    

    magma_int_t M, N, n2, lda, ldda, min_mn, info;
    magma_int_t ione     = 1; 
    magma_int_t ISEED[4] = {0,0,0,1};
    magma_int_t batchCount = 1;

    magma_opts opts;
    parse_opts( argc, argv, &opts );
    //opts.lapack |= opts.check; 

    batchCount = opts.batchcount ;
    magma_int_t matrixSize;  
    magma_int_t columns;
    FILE        *fp ;
    char fname[200];
    int j,k,batchoffset;
    
    printf("BatchCount      M     N     CPU GFlop/s (ms)    MAGMA GFlop/s (ms)  CUBLAS GFlop/s (ms)  ||PA-LU||/(||A||*N)\n");
    printf("=========================================================================\n");
    for( int i = 0; i < opts.ntest; ++i ) {
    
      for( int iter = 0; iter < opts.niter; ++iter ) {
            
            M = opts.msize[i];
            N = opts.nsize[i];
            min_mn = min(M, N);
            lda    = M;
            n2     = lda*N * batchCount;
            ldda   = ((M+31)/32)*32;
            //ldda = M;
            gflops = FLOPS_ZGETRF( M, N ) / 1e9 * batchCount;
            matrixSize =  ldda * N;  
            

            TESTING_MALLOC_CPU(    ipiv, magma_int_t,     min_mn * batchCount);
            TESTING_MALLOC_CPU(    h_A,  magmaDoubleComplex, n2     );
            TESTING_MALLOC_PIN( h_R,  magmaDoubleComplex, n2     );
            TESTING_MALLOC_DEV(  dA_magma,  magmaDoubleComplex, ldda*N * batchCount);
            //TESTING_MALLOC_DEV(  dA_cublas,  magmaDoubleComplex, ldda*N * batchCount);
            TESTING_MALLOC_DEV(  dipiv_magma,  magma_int_t, min_mn * batchCount);
            TESTING_MALLOC_DEV(  dinfo_magma,  magma_int_t, batchCount);
            TESTING_MALLOC_DEV(  dipiv_cublas,  magma_int_t, min_mn * batchCount);
            TESTING_MALLOC_DEV(  dinfo_cublas,  magma_int_t, batchCount);

            magma_malloc((void**)&dA_array, batchCount * sizeof(*dA_array));
            magma_malloc((void**)&dipiv_array, batchCount * sizeof(*dipiv_array));

            /* Initialize the matrix */
            lapackf77_zlarnv( &ione, ISEED, &n2, h_A );
#if 0
            // introduce error in matrix "batchid" making column "col" zero.
            magma_int_t batchid=1;
            magma_int_t col = 35;
            for(int i=0; i<M; i++)
                h_A[i+col*lda + batchid*lda*N] = MAGMA_Z_ZERO;
#endif



//#define PRINTMAT
    
#ifdef PRINTMAT 
            printf("Writing input matrix in orig.txt ...\n");
            for(int i=0; i<batchCount; i++)
            {
                sprintf(fname,"orig_%d",i);
                fp = fopen (fname, "w") ;
                if ( fp == NULL ) { printf("Couldn't open output file\n"); exit(1); }
                batchoffset = i*lda*N;
                for (j=0; j < N; j++) {
                    for (k=0; k < M; k++) {
                        #if defined(PRECISION_z) || defined(PRECISION_c)
                        fprintf(fp, "%5d %5d %11.8f %11.8f\n", k+1, j+1,
                                h_A[batchoffset+k+j*lda].x, h_A[batchoffset+k+j*lda].y);
                        #else
                        fprintf(fp, "%5d %5d %25.12e\n", k+1, j+1, h_A[batchoffset+k+j*lda]);
                        #endif
                    }
                }
                fclose( fp ) ;
            }
#endif  



            columns = N * batchCount;
            lapackf77_zlacpy( MagmaUpperLowerStr, &M, &columns, h_A, &lda, h_R, &lda );
            magma_zsetmatrix( M, columns, h_R, lda, dA_magma, ldda );
            


            /* ====================================================================
               Performs operation using MAGMA
               =================================================================== */
            zset_pointer(dA_array, dA_magma, ldda, 0, 0, ldda*N, batchCount);
            magma_time = magma_sync_wtime(0);
            info = magma_zgetrf_nopiv_batched( M, N, dA_array, ldda, dinfo_magma, batchCount);
            magma_time = magma_sync_wtime(0) - magma_time;
            magma_perf = gflops / magma_time;
            // check correctness of results throught "dinfo_magma" and correctness of argument throught "info"
            magma_int_t *cpu_info = (magma_int_t*) malloc(batchCount*sizeof(magma_int_t));
            magma_getvector( batchCount, sizeof(magma_int_t), dinfo_magma, 1, cpu_info, 1);
            for(int i=0; i<batchCount; i++)
            {
                if(cpu_info[i] != 0 ){
                    printf("magma_zgetrf_batched matrix %d returned internal error %d\n",i, (int)cpu_info[i] );
                }
            }
            if (info != 0)
                printf("magma_zgetrf_batched returned argument error %d: %s.\n", (int) info, magma_strerror( info ));

            
#ifdef PRINTMAT 

            magma_zgetmatrix( M, N*batchCount, dA_magma, ldda, h_R, lda );
            printf("Writing input matrix in mag.txt ...\n");
            for(int i=0; i<batchCount; i++)
            {
                sprintf(fname,"mag_%d",i);
                fp = fopen (fname, "w") ;
                if ( fp == NULL ) { printf("Couldn't open output file\n"); exit(1); }
                batchoffset = i*lda*N;
                for (j=0; j < N; j++) {
                    for (k=0; k < M; k++) {
                        #if defined(PRECISION_z) || defined(PRECISION_c)
                        fprintf(fp, "%5d %5d %11.8f %11.8f\n", k+1, j+1,
                                h_R[batchoffset+k+j*lda].x, h_R[batchoffset+k+j*lda].y);
                        #else
                        fprintf(fp, "%5d %5d %25.12e\n", k+1, j+1, h_R[batchoffset+k+j*lda]);
                        #endif
                    }
                }
                fclose( fp ) ;
            }
            lapackf77_zlacpy( MagmaUpperLowerStr, &M, &columns, h_A, &lda, h_R, &lda );

#endif  


            /* ====================================================================
               Performs operation using CUBLAS
               =================================================================== */
/*
            magma_zsetmatrix( M, columns, h_R, lda, dA_cublas,  ldda );
            zset_pointer(dA_array, dA_cublas, ldda, 0, 0, matrixSize, batchCount);

            cublas_time = magma_sync_wtime(0);
            if(M == N )
            {
                cublasZgetrfBatched( handle, N, dA_array, ldda, dipiv_cublas,  dinfo_cublas, batchCount);
                cublas_enable = 1;
            }
            else
            {
                printf("M != N, CUBLAS required M == N ; CUBLAS is disabled\n");
            }
            cublas_time = magma_sync_wtime(0) - cublas_time;
            cublas_perf = gflops / cublas_time;
*/

            /* =====================================================================
               Performs operation using LAPACK
               =================================================================== */
            if ( opts.lapack ) {
                cpu_time = magma_wtime();
                //#pragma unroll
                for(int i=0; i<batchCount; i++)
                {
                  lapackf77_zgetrf(&M, &N, h_A + i*lda*N, &lda, ipiv + i * min_mn, &info);
                }
                cpu_time = magma_wtime() - cpu_time;
                cpu_perf = gflops / cpu_time;
                if (info != 0)
                    printf("lapackf77_zgetrf returned error %d: %s.\n",
                           (int) info, magma_strerror( info ));

#ifdef PRINTMAT 
            printf("Writing input matrix in lpk.txt ...\n");
            for(int i=0; i<batchCount; i++)
            {
                sprintf(fname,"lpk_%d",i);
                fp = fopen (fname, "w") ;
                if ( fp == NULL ) { printf("Couldn't open output file\n"); exit(1); }
                batchoffset = i*lda*N;
                for (j=0; j < N; j++) {
                    for (k=0; k < M; k++) {
                        #if defined(PRECISION_z) || defined(PRECISION_c)
                        fprintf(fp, "%5d %5d %11.8f %11.8f\n", k+1, j+1,
                                h_A[batchoffset+k+j*lda].x, h_A[batchoffset+k+j*lda].y);
                        #else
                        fprintf(fp, "%5d %5d %25.12e\n", k+1, j+1, h_A[batchoffset+k+j*lda]);
                        #endif
                    }
                }
                fclose( fp ) ;
            }
#endif  


            }
            




            /* =====================================================================
               Check the factorization
               =================================================================== */
            if ( opts.lapack ) {
                printf("%10d   %5d  %5d     %7.2f (%7.2f)   %7.2f (%7.2f)    %7.2f (%7.2f)",
                       (int) batchCount, (int) M, (int) N, cpu_perf, cpu_time*1000., magma_perf, magma_time*1000., cublas_perf*cublas_enable, cublas_time*1000.*cublas_enable  );
            }
            else {
                printf("%10d   %5d  %5d     ---   (  ---  )   %7.2f (%7.2f)    %7.2f (%7.2f)",
                       (int) batchCount, (int) M, (int) N, magma_perf, magma_time*1000., cublas_perf*cublas_enable, cublas_time*1000.*cublas_enable );
            }

            double err = 0.0;
            if ( opts.check ) {
               
                // initialize ipiv to 1,2,3,4,5,6
                for(int i=0; i<batchCount; i++)
                {
                    for(int k=0;k<min_mn; k++){
                        ipiv[i*min_mn+k] = k+1;
                    }
                }


                magma_zgetmatrix( M, N*batchCount, dA_magma, ldda, h_A, lda );
                int stop=0;
                for(int i=0; i<batchCount; i++)
                {
                    /*
                    for(int k=0;k<min_mn; k++){
                        if(ipiv[i*min_mn+k] < 1 || ipiv[i*min_mn+k] > M )
                        {
                                printf("error for matrix %d ipiv @ %d = %d\n",i,k,ipiv[i*min_mn+k]);
                                stop=1;
                        }
                    }
                  if(stop==1){
                      err=-1.0;
                      break;
                  }
                  */


                  error = get_LU_error( M, N, h_R + i * lda*N, lda, h_A + i * lda*N, ipiv + i * min_mn);                 
                  if ( isnan(error) || isinf(error) ) {
                      err = error;
                      break;
                  }
                  err = max(fabs(error),err);
                }
                 printf("   %8.2e\n", err );
            }
            else {
                printf("     ---  \n");
            }
            
            TESTING_FREE_CPU( ipiv );
            TESTING_FREE_CPU( h_A );
            TESTING_FREE_PIN( h_R );
            TESTING_FREE_DEV( dA_magma );
            TESTING_FREE_DEV( dA_cublas);
            TESTING_FREE_DEV( dinfo_magma );
            TESTING_FREE_DEV( dipiv_magma );
            TESTING_FREE_DEV( dipiv_cublas );
            TESTING_FREE_DEV( dinfo_cublas );
            TESTING_FREE_DEV( dipiv_array );
            TESTING_FREE_DEV( dA_array );
            free(cpu_info);
        }
        if ( opts.niter > 1 ) {
            printf( "\n" );
        }
    }
    TESTING_FINALIZE();
    return 0;
}