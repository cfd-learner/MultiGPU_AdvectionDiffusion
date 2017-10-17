//
//  main.c
//  Burgers3d-MPI-GPU
//
//  Created by Manuel Diaz on 7/26/17.
//  Copyright © 2016 Manuel Diaz. All rights reserved.
//

#include "BurgersMPI.h"

/*********************************************/
/* A method for checking error in CUDA calls */
/*********************************************/
#define checkCuda(error) __checkCuda(error, __FILE__, __LINE__)
inline void __checkCuda(cudaError_t error, const char *file, const int line)
{
#if defined(DEBUG) || defined(_DEBUG)
	if (error != cudaSuccess)
	{
		printf("checkCuda error at %s:%i: %s\n", file, line, cudaGetErrorString(cudaGetLastError()));
		exit(-1);
	}
#endif
	return;
}

/**********************/
/* Main program entry */
/**********************/
int main(int argc, char** argv)
{
	REAL tEnd, CFL, L, W, H;
    unsigned int Nx, Ny, Nz, blockX, blockY, blockZ;
    int rank, numberOfProcesses;
    
    if (argc == 12)
    {
        tEnd = atof(argv[1]);		// Final time
        CFL= atof(argv[2]);			// The stability parameter
        L = atof(argv[3]);			// domain lenght
        W = atof(argv[4]);			// domain width
        H = atof(argv[5]);			// domain height
        Nx = atoi(argv[6]);			// number cells in x-direction
        Ny = atoi(argv[7]);			// number cells in y-direction
        Nz = atoi(argv[8]);			// number cells in z-direction
        blockX = atoi(argv[9]); 	// block size in the i-direction
        blockY = atoi(argv[10]); 	// block size in the j-direction
        blockZ = atoi(argv[11]); 	// block size in the k-direction
    }
    else
    {
        printf("Usage: %s tEnd CFL L W H Nx Ny NZ block_x block_y block_z\n", argv[0]);
        exit(1);
    }

    InitializeMPI(&argc, &argv, &rank, &numberOfProcesses);
	AssignDevices(rank);
	ECCCheck(rank);

	// Define Constanst
    const REAL   dx = L/(Nx-1); 				// dx, cell size
    const REAL   dy = W/(Ny-1); 				// dy, cell size
    const REAL   dz = H/(Nz-1); 				// dy, cell size
    const unsigned int _Nz = Nz/numberOfProcesses;	// Decompose along the z-axis
    const unsigned int  NZ = Nz+2*RADIUS;
    const unsigned int _NZ =_Nz+2*RADIUS;
    const unsigned int dt_size= sizeof(REAL);	// Data size
    printf("dx: %g, dy: %g, dz: %g, final time: %g\n\n",dx,dy,dz,tEnd);

    // Initialize solution arrays
    REAL *h_u; h_u = (REAL*)malloc(sizeof(REAL)*Nx*Ny*NZ);

	Init_domain(3,h_u,dx,dy,dz,Nx,Ny,NZ); 
	if (DEBUG) printf("Domain Initialized rank %d\n",rank);

	// Write solution to file
	if (rank == 0) 
	{
    	SaveBinary3D(h_u,Nx,Ny,NZ,"initial.bin");
    	printf("IC saved in Host rank %d\n", rank);
	}

	// Allocate subdomains and transfer buffers in host (building as pinned memory)
	REAL *h_s_recvbuff[numberOfProcesses];
	REAL *h_s_u; h_s_u = (REAL*)malloc(sizeof(REAL)*Nx*Ny*_NZ);
	checkCuda(cudaHostAlloc((void**)&h_s_u, sizeof(REAL)*Nx*Ny*_NZ, cudaHostAllocPortable));

	if (rank == 0)
	{
		for (int i = 0; i < numberOfProcesses; i++)
		{
		    h_s_recvbuff[i] = (REAL*)malloc(sizeof(REAL)*Nx*Ny*_NZ);
		    checkCuda(cudaHostAlloc((void**)&h_s_recvbuff[i], sizeof(REAL)*Nx*Ny*_NZ, cudaHostAllocPortable));
		}
	}

	// Initialize subdomains
    Init_subdomain(h_u,h_s_u,rank,Nx,Ny,_Nz);
    if (DEBUG) printf("SubDomain %d Initialized\n", rank);

	// Allocate left/right receive/send buffers
	REAL *l_u_send_buffer; l_u_send_buffer = (REAL*)malloc(sizeof(REAL)*Nx*Ny*RADIUS);
	REAL *r_u_send_buffer; r_u_send_buffer = (REAL*)malloc(sizeof(REAL)*Nx*Ny*RADIUS);
	REAL *l_u_recv_buffer; l_u_recv_buffer = (REAL*)malloc(sizeof(REAL)*Nx*Ny*RADIUS);
	REAL *r_u_recv_buffer; r_u_recv_buffer = (REAL*)malloc(sizeof(REAL)*Nx*Ny*RADIUS);
	checkCuda(cudaHostAlloc((void**)&l_u_send_buffer, sizeof(REAL)*Nx*Ny*RADIUS, cudaHostAllocPortable));
	checkCuda(cudaHostAlloc((void**)&r_u_send_buffer, sizeof(REAL)*Nx*Ny*RADIUS, cudaHostAllocPortable));
	checkCuda(cudaHostAlloc((void**)&l_u_recv_buffer, sizeof(REAL)*Nx*Ny*RADIUS, cudaHostAllocPortable));
	checkCuda(cudaHostAlloc((void**)&r_u_recv_buffer, sizeof(REAL)*Nx*Ny*RADIUS, cudaHostAllocPortable));
	if (DEBUG) printf("Send/Receive buffers allocated in rank %d\n", rank);

	// Initialize GPU streams
	cudaStream_t computeStream; checkCuda(cudaStreamCreate(&computeStream));
	cudaStream_t r_send_stream; checkCuda(cudaStreamCreate(&r_send_stream));
	cudaStream_t l_send_stream; checkCuda(cudaStreamCreate(&l_send_stream));
	cudaStream_t r_recv_stream; checkCuda(cudaStreamCreate(&r_recv_stream));
	cudaStream_t l_recv_stream; checkCuda(cudaStreamCreate(&l_recv_stream));
	if (DEBUG) printf("Streams created in rank %d\n", rank);

	// GPU Memory Operations
	size_t pitch_bytes, pitch_gc_bytes;
	REAL *d_s_u;  checkCuda(cudaMallocPitch((void**)&d_s_u , &pitch_bytes, sizeof(REAL)*Nx, Ny*_NZ ));
	REAL *d_s_uo; checkCuda(cudaMallocPitch((void**)&d_s_uo, &pitch_bytes, sizeof(REAL)*Nx, Ny*_NZ ));
	REAL *d_s_Lu; checkCuda(cudaMallocPitch((void**)&d_s_Lu, &pitch_bytes, sizeof(REAL)*Nx, Ny*_NZ ));
	REAL *d_l_u_send_buffer; checkCuda(cudaMallocPitch((void**)&d_l_u_send_buffer, &pitch_gc_bytes, sizeof(REAL)*Nx, Ny*RADIUS));
	REAL *d_r_u_send_buffer; checkCuda(cudaMallocPitch((void**)&d_r_u_send_buffer, &pitch_gc_bytes, sizeof(REAL)*Nx, Ny*RADIUS));
	REAL *d_l_u_recv_buffer; checkCuda(cudaMallocPitch((void**)&d_l_u_recv_buffer, &pitch_gc_bytes, sizeof(REAL)*Nx, Ny*RADIUS));
	REAL *d_r_u_recv_buffer; checkCuda(cudaMallocPitch((void**)&d_r_u_recv_buffer, &pitch_gc_bytes, sizeof(REAL)*Nx, Ny*RADIUS));
	if (DEBUG) printf("Pitched memory arrays created in GPU %d\n", rank);

	// Copy subdomains from host to device and get walltime
	double HtD_timer = 0.;

	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
	HtD_timer -= MPI_Wtime();
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

    checkCuda(cudaMemcpy2D(d_s_u, pitch_bytes, h_s_u, sizeof(REAL)*Nx, sizeof(REAL)*Nx, Ny*_NZ, cudaMemcpyDefault));

    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
	HtD_timer += MPI_Wtime();
	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
	if (DEBUG) printf("Memory copied to GPU %d\n", rank);

	unsigned int pitch = pitch_bytes/dt_size; printf("pitch: %d\n",pitch);
	unsigned int gc_pitch = pitch_gc_bytes/dt_size; printf("gc_pitch: %d\n",gc_pitch);

    // GPU kernel launch parameters
	dim3 threadsPerBlock3D(blockX, blockY, blockZ);
	unsigned int blocksInX = getBlock( Nx, blockX);
	unsigned int blocksInY = getBlock( Ny, blockY);
	unsigned int blocksInZ = getBlock(_NZ, blockZ);
	dim3 numBlocks3D(blocksInX, blocksInY, blocksInZ);

	// Halo communication
	dim3 threadsPerHalo2D_XY(blockX,blockY,1);
	dim3 threadsPerHalo3D_XY(blockX,blockY,RADIUS);
	dim3 numBlocksHalo2D_XY(blocksInX,blocksInY,1);

	// XY plane
    dim3 threadsPerBlock2D_XY(blockX,blockY,1); // initialization for C++
    dim3 numBlocks2D_XY(blocksInX,blocksInY,1); // initialization for C++

    // XZ plane
    dim3 threadsPerBlock2D_XZ(blockX,1,blockZ); // initialization for C++
    dim3 numBlocks2D_XZ(blocksInX,1,blocksInZ); // initialization for C++

    // YZ plane
    dim3 threadsPerBlock2D_YZ(1,blockY,blockZ); // initialization for C++
    dim3 numBlocks2D_YZ(1,blocksInY,blocksInZ); // initialization for C++

	//MPI_Status status;
	MPI_Status status[numberOfProcesses];
	MPI_Request gather_send_request[numberOfProcesses];
	MPI_Request r_u_send_request[numberOfProcesses], l_u_send_request[numberOfProcesses], r_u_recv_request[numberOfProcesses], l_u_recv_request[numberOfProcesses];

	// Initialize time variables
    REAL dt =0; 
    REAL it =0;
    REAL  t =0;

	if (DEBUG) printf("Begin computation loop in rank %d\n", rank);
	double compute_timer = 0.;

	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    compute_timer -= MPI_Wtime();
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

	// Call WENO-RK solver
    while (t < tEnd)
	{
		// Update/correct time step
        dt=CFL*dx/1.0; if ((t+dt)>tEnd){ dt=tEnd-t; } 
    
        // Update time and iteration counter
        t+=dt; it+=1;

		// Runge Kutta Step 0
		checkCuda(cudaMemcpy2DAsync(d_s_uo, pitch_bytes, d_s_u, pitch_bytes, dt_size*Nx, Ny*_NZ, cudaMemcpyDefault, computeStream));

		// Set memory of temporal variables to zero
		checkCuda(cudaMemset2DAsync(d_s_Lu, pitch_bytes, 0, dt_size*Nx, Ny*_NZ, computeStream));

		// Runge Kutta Steps 1-3
		for (unsigned int step = 1; step <= 3; step++) // 3 runge kutta steps!!
		{
	       	// Compute right boundary on devices 0-2, send to devices 1-(n-1)
			if (rank < numberOfProcesses-1)
			{
				unsigned int kstart = _Nz;
				unsigned int kstop = _Nz+RADIUS;

				Compute_Adv_x_Async(numBlocks2D_YZ, threadsPerBlock2D_YZ, r_send_stream, pitch, Nx, Ny, _NZ, kstart, kstop, dx, d_s_u, d_s_Lu);
				Compute_Adv_y_Async(numBlocks2D_XZ, threadsPerBlock2D_XZ, r_send_stream, pitch, Nx, Ny, _NZ, kstart, kstop, dy, d_s_u, d_s_Lu);
				Compute_Adv_z_Async(numBlocks2D_XY, threadsPerBlock2D_XY, r_send_stream, pitch, Nx, Ny, _NZ, kstart, kstop, dz, d_s_u, d_s_Lu);

				CopyBoundaryRegionToGhostCellAsync(numBlocksHalo2D_XY, threadsPerHalo2D_XY, r_send_stream, d_s_Lu, d_r_u_send_buffer, Nx, Ny, _NZ, pitch, gc_pitch, 0);

				checkCuda(cudaMemcpy2DAsync(r_u_send_buffer, dt_size*Nx, d_r_u_send_buffer, pitch_gc_bytes, dt_size*Nx, Ny*RADIUS, cudaMemcpyDefault, r_send_stream));
				checkCuda(cudaStreamSynchronize(r_send_stream));
				// if (rank==0) printGPUmem(1, 1, r_send_stream, pitch, Nx, Ny, _NZ, 0, _NZ, d_s_Lu);
				// if (rank==0) printGPUmem(1, 1, r_send_stream, pitch, Nx, Ny, RADIUS, 0, RADIUS, d_r_u_send_buffer);

				MPI_CHECK(MPI_Isend(r_u_send_buffer, Nx*Ny*RADIUS, MPI_CUSTOM_REAL, rank+1, 1, MPI_COMM_WORLD, &r_u_send_request[rank]));
			}
			if (rank > 0)
			{
				unsigned int kstart = RADIUS;
				unsigned int kstop = 2*RADIUS;

				Compute_Adv_x_Async(numBlocks2D_YZ, threadsPerBlock2D_YZ, l_send_stream, pitch, Nx, Ny, _NZ, kstart, kstop, dx, d_s_u, d_s_Lu);
				Compute_Adv_y_Async(numBlocks2D_XZ, threadsPerBlock2D_XZ, l_send_stream, pitch, Nx, Ny, _NZ, kstart, kstop, dy, d_s_u, d_s_Lu);
				Compute_Adv_z_Async(numBlocks2D_XY, threadsPerBlock2D_XY, l_send_stream, pitch, Nx, Ny, _NZ, kstart, kstop, dz, d_s_u, d_s_Lu);

				CopyBoundaryRegionToGhostCellAsync(numBlocksHalo2D_XY, threadsPerHalo2D_XY, l_send_stream, d_s_Lu, d_l_u_send_buffer, Nx, Ny, _NZ, pitch, gc_pitch, 1);

				checkCuda(cudaMemcpy2DAsync(l_u_send_buffer, dt_size*Nx, d_l_u_send_buffer, pitch_gc_bytes, dt_size*Nx, Ny*RADIUS, cudaMemcpyDefault, l_send_stream));
				checkCuda(cudaStreamSynchronize(l_send_stream));
				// if (rank==1) printGPUmem(1, 1, l_send_stream, pitch, Nx, Ny, _NZ, 0, _NZ, d_s_Lu);
				// if (rank==1) printGPUmem(1, 1, l_send_stream, pitch, Nx, Ny, RADIUS, 0, RADIUS, d_l_u_send_buffer);

				MPI_CHECK(MPI_Isend(l_u_send_buffer, Nx*Ny*RADIUS, MPI_CUSTOM_REAL, rank-1, 5, MPI_COMM_WORLD, &l_u_send_request[rank]));
			}

			// Compute inner points for device 0
			if (rank == 0)
			{
				unsigned int kstart = RADIUS;
				unsigned int kstop = _Nz;

				Compute_Adv_x_Async(numBlocks2D_YZ, threadsPerBlock2D_YZ, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dx, d_s_u, d_s_Lu);
				Compute_Adv_y_Async(numBlocks2D_XZ, threadsPerBlock2D_XZ, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dy, d_s_u, d_s_Lu);
				Compute_Adv_z_Async(numBlocks2D_XY, threadsPerBlock2D_XY, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dz, d_s_u, d_s_Lu);
			}
			// Compute inner points for device 1 and (n-2)
			if (rank > 0 && rank < numberOfProcesses-1)
			{
				unsigned int kstart = 2*RADIUS;
				unsigned int kstop = _Nz;

				Compute_Adv_x_Async(numBlocks2D_YZ, threadsPerBlock2D_YZ, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dx, d_s_u, d_s_Lu);
				Compute_Adv_y_Async(numBlocks2D_XZ, threadsPerBlock2D_XZ, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dy, d_s_u, d_s_Lu);
				Compute_Adv_z_Async(numBlocks2D_XY, threadsPerBlock2D_XY, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dz, d_s_u, d_s_Lu);
			}
			// Compute inner points for device (n-1)
			if (rank == numberOfProcesses-1)
			{
				unsigned int kstart = 2*RADIUS;
				unsigned int kstop = _Nz+RADIUS;

				Compute_Adv_x_Async(numBlocks2D_YZ, threadsPerBlock2D_YZ, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dx, d_s_u, d_s_Lu);
				Compute_Adv_y_Async(numBlocks2D_XZ, threadsPerBlock2D_XZ, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dy, d_s_u, d_s_Lu);
				Compute_Adv_z_Async(numBlocks2D_XY, threadsPerBlock2D_XY, computeStream, pitch, Nx, Ny, _NZ, kstart, kstop, dz, d_s_u, d_s_Lu);
			}

			// Receive data from 0-2
			if (rank < numberOfProcesses-1)
			{
				MPI_CHECK(MPI_Irecv(r_u_recv_buffer, Nx*Ny*RADIUS, MPI_CUSTOM_REAL, rank+1, 5, MPI_COMM_WORLD, &r_u_recv_request[rank]));
			}
			// Receive data from 1-(n-1)
			if (rank > 0)
			{
				MPI_CHECK(MPI_Irecv(l_u_recv_buffer, Nx*Ny*RADIUS, MPI_CUSTOM_REAL, rank-1, 1, MPI_COMM_WORLD, &l_u_recv_request[rank]));
			}

			// Receive data from 0-2
			if (rank < numberOfProcesses-1)
			{
				MPI_CHECK(MPI_Wait(&r_u_recv_request[rank], MPI_STATUS_IGNORE));

				checkCuda(cudaMemcpy2DAsync(d_r_u_recv_buffer, pitch_gc_bytes, r_u_recv_buffer, dt_size*Nx, dt_size*Nx, Ny*RADIUS, cudaMemcpyDefault, r_recv_stream));
				CopyGhostCellToBoundaryRegionAsync(numBlocksHalo2D_XY, threadsPerHalo2D_XY, r_recv_stream, d_s_Lu, d_r_u_recv_buffer, Nx, Ny, _NZ, pitch, gc_pitch, 0);
			}
			// Receive data from 1-(n-1)
			if (rank > 0)
			{
				MPI_CHECK(MPI_Wait(&l_u_recv_request[rank], MPI_STATUS_IGNORE));

				checkCuda(cudaMemcpy2DAsync(d_l_u_recv_buffer, pitch_gc_bytes, l_u_recv_buffer, dt_size*Nx, dt_size*Nx, Ny*RADIUS, cudaMemcpyDefault, l_recv_stream));
				CopyGhostCellToBoundaryRegionAsync(numBlocksHalo2D_XY, threadsPerHalo2D_XY, l_recv_stream, d_s_Lu, d_l_u_recv_buffer, Nx, Ny, _NZ, pitch, gc_pitch, 1);
			}

			if (rank < numberOfProcesses-1)
			{
				MPI_CHECK(MPI_Wait(&r_u_send_request[rank], MPI_STATUS_IGNORE));
			}
			if (rank > 0)
			{
				MPI_CHECK(MPI_Wait(&l_u_send_request[rank], MPI_STATUS_IGNORE));
			}

			// No need to swap pointers
			Compute_sspRK3Async(numBlocks3D, threadsPerBlock3D, computeStream, step, pitch, Nx, Ny, _NZ, dt, d_s_u, d_s_uo, d_s_Lu);
			checkCuda(cudaDeviceSynchronize());
		}
	}

	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
	compute_timer += MPI_Wtime();
	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

	// Report final dt and iterations
	if (rank == 0) printf("dt: %g, iterations: %g, final time: %g\n\n",dt,it,t);

	// Copy data from device to host
	double DtH_timer = 0;

	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
	DtH_timer -= MPI_Wtime();
	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));

	checkCuda(cudaMemcpy2D(h_s_u, dt_size*Nx, d_s_u, pitch_bytes, dt_size*Nx, Ny*_NZ, cudaMemcpyDefault));

	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
	DtH_timer += MPI_Wtime();
	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
	if (DEBUG) printf("Memory copied back to Host %d\n", rank);

	// Gather results from subdomains
	MPI_CHECK(MPI_Isend(h_s_u, Nx*Ny*_NZ, MPI_CUSTOM_REAL, 0, 0, MPI_COMM_WORLD, &gather_send_request[rank]));
	if (rank == 0)
	{
		for (int i = 0; i < numberOfProcesses; i++)
		{
			MPI_CHECK(MPI_Recv(h_s_recvbuff[i], Nx*Ny*_NZ, MPI_CUSTOM_REAL, i, 0, MPI_COMM_WORLD, &status[rank]));
			Merge_domains(h_s_recvbuff[i], h_u, i, Nx, Ny, _Nz);
		}
	}
	if (DEBUG) printf("Subdomains merged %d\n", rank);

	// Write solution to file
	if (rank == 0) 
	{
    	if (WRITE) SaveBinary3D(h_u,Nx,Ny,NZ,"result.bin");
    	if (DEBUG) printf("Solution saved in Host rank %d\n", rank);
	}

	// Final Report
	if (rank == 0)
	{
		float gflops = CalcGflops(compute_timer, it, Nx, Ny, NZ);
		PrintSummary("Burgers-3D MPI-GPU-WENO5", "Pitched Memory", compute_timer, HtD_timer, DtH_timer, gflops, it, Nx, Ny, NZ);
	}

	FinalizeMPI();

	// Free device memory
	checkCuda(cudaFree(d_s_u ));
	checkCuda(cudaFree(d_s_uo));
	checkCuda(cudaFree(d_s_Lu));
	checkCuda(cudaFree(d_r_u_send_buffer));
	checkCuda(cudaFree(d_l_u_send_buffer));
	checkCuda(cudaFree(d_r_u_recv_buffer));
	checkCuda(cudaFree(d_l_u_recv_buffer));

	// Free host memory
	checkCuda(cudaFreeHost(h_s_u));

	if (rank == 0)
	{
		for (int i = 0; i < numberOfProcesses; i++)
		{
			checkCuda(cudaFreeHost(h_s_recvbuff[i]));
		}
	}

	checkCuda(cudaFreeHost(l_u_send_buffer));
	checkCuda(cudaFreeHost(l_u_recv_buffer));
	checkCuda(cudaFreeHost(r_u_send_buffer));
	checkCuda(cudaFreeHost(r_u_recv_buffer));

	// Force Reset Device
	checkCuda(cudaDeviceReset());

	// Free memory on all hosts
	free(h_u);
	return 0;
}
