#include "qemu-common.h"
#include "qemu/iov.h"
#include "qemu/error-report.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-qcuda.h"
#include <sys/mman.h>

#ifdef CONFIG_CUDA
#include <cuda.h>
#include <cuda_runtime.h>
#include <builtin_types.h>
#endif

#if 1
#define pfunc() printf("### %s at line %d\n", __func__, __LINE__)
#else
#define pfunc()
#endif

#if 1
#define ptrace(fmt, arg...) \
	printf("    " fmt, ##arg)
#else
#define ptrace(fmt, arg...)
#endif


#include "../../../qcu-driver/qcuda_common.h"

#define error(fmt, arg...) \
	error_report("file %s ,line %d ,ERROR: "fmt, __FILE__, __LINE__, ##arg)

#ifndef MIN
#define MIN(a,b) ({ ((a)<(b))? (a):(b) })
#endif

#define VIRTHM_DEV_PATH "/dev/vf0"
#define MAP_MAX_LENGTH (1956480*1024) //1.9 GB

uint32_t BLOCK_SIZE;

char *deviceSpace = NULL;
uint32_t deviceSpaceSize = 0;

//void *testdev;
//char *buf; //cocotion test

static void* gpa_to_hva(uint64_t pa)
{
	MemoryRegionSection section;

	section = memory_region_find(get_system_memory(), (ram_addr_t)pa, 1);
	if ( !int128_nz(section.size) || !memory_region_is_ram(section.mr)){
		error("addr %p in rom\n", (void*)pa);
		return 0;
	}

	return (memory_region_get_ram_ptr(section.mr) + section.offset_within_region);
}

#ifdef CONFIG_CUDA
CUdevice cudaDeviceCurrent;
CUcontext cudaContext;
// CUmodule cudaModule;

int cudaContext_count;

#define cudaFunctionMaxNum 8
// CUfunction cudaFunction[cudaFunctionMaxNum];
// uint32_t cudaFunctionId[cudaFunctionMaxNum];
uint32_t cudaFunctionNum;

#define cudaEventMaxNum 16
cudaEvent_t cudaEvent[cudaEventMaxNum];
uint32_t cudaEventNum;

#define cudaStreamMaxNum 32
cudaStream_t cudaStream[cudaStreamMaxNum];
uint32_t cudaStreamNum;

typedef struct kernelInfo {
	void *fatBin;
	char *functionName;
	uint32_t funcId;
	// VirtioQCArg theArg;
} kernelInfo;

typedef struct cudaDev {
	CUdevice device;
	CUcontext context;
	uint32_t cudaFunctionId[cudaFunctionMaxNum];
	CUfunction cudaFunction[cudaFunctionMaxNum];
	CUmodule module;
	int kernelsLoaded;
	// cudaStream_t stream;
} cudaDev;

int totalDevices;
cudaDev *cudaDevices;
cudaDev zeroedDevice;
kernelInfo devicesKernels[cudaFunctionMaxNum];

#define cudaError(err) __cudaErrorCheck(err, __LINE__)
static inline void __cudaErrorCheck(cudaError_t err, const int line)
{
	char *str;
	if ( err != cudaSuccess )
	{
		str = (char*)cudaGetErrorString(err);
		error_report("CUDA Runtime API error = %04d \"%s\" line %d\n", err, str, line);
	}
}


#define cuError(err)  __cuErrorCheck(err, __LINE__)
static inline void __cuErrorCheck(CUresult err, const int line)
{
	char *str;
	if ( err != CUDA_SUCCESS )
	{
		cuGetErrorName(err, (const char**)&str);
		error_report("CUDA Runtime API error = %04d \"%s\" line %d\n", err, str, line);
	}
}

////////////////////////////////////////////////////////////////////////////////
///	Module & Execution control (driver API)
////////////////////////////////////////////////////////////////////////////////

static void qcu_cudaRegisterFatBinary(VirtioQCArg *arg)
{
	uint32_t i;
	pfunc();

	// for(i=0; i<cudaFunctionMaxNum; i++)
	// 	memset(&cudaFunction[i], 0, sizeof(CUfunction));

	for(i=0; i<cudaEventMaxNum; i++)
		memset(&cudaEvent[i], 0, sizeof(cudaEvent_t));

	for(i=0; i<cudaStreamMaxNum; i++)
		memset(&cudaStream[i], 0, sizeof(cudaStream_t));

	cuError( cuInit(0) );
	cuError( cuDeviceGetCount(&totalDevices) );
	cudaDevices = (cudaDev *) malloc(totalDevices * sizeof(cudaDev));
	memset(&zeroedDevice, 0, sizeof(cudaDev));

	i = totalDevices;
	// the last created context is the one used & associated with the device
	// so do this in reverse order
	// while(i-- != 0)
	while(i-- != 0)
	{
		printf("creating context for device %d\n", i);
		memset(&cudaDevices[i], 0, sizeof(cudaDev));
		cuError( cuDeviceGet(&cudaDevices[i].device, i) );
		cuError( cuCtxCreate(&cudaDevices[i].context, 0, cudaDevices[i].device) );
		memset(&cudaDevices[i].cudaFunction, 0, sizeof(CUfunction) * cudaFunctionMaxNum);
		cudaDevices[i].kernelsLoaded = 0;
		// cuError( cudaStreamCreate(&cudaDevices[i].stream) );
	}

	cudaDeviceCurrent = cudaDevices[0].device; //used when calling cudaGetDevice

	// cuError( cuDeviceGet(&cudaDeviceCurrent, 0) );
	// cuError( cuCtxCreate(&cudaContext, 0, cudaDeviceCurrent) );
//	cudaSetDevice(0);
	cudaFunctionNum = 0;
	cudaEventNum = 0;

	cudaStreamNum = 1;

	cudaContext_count = 1;
}

static void qcu_cudaUnregisterFatBinary(VirtioQCArg *arg)
{
	uint32_t i;
	pfunc();

	for(i=0; i<cudaEventMaxNum; i++)
	{
		if( cudaEvent[i] != 0 ){
			cudaError( cudaEventDestroy(cudaEvent[i]));
		}
	}

	for(i = 0; i < totalDevices; i++)
	{
		// get rid of default context if any
		// when a device is reset there will be no context
		if( memcmp( &zeroedDevice, &cudaDevices[i], sizeof(cudaDev) ) != 0 )
		{
			printf("Destroying context for dev %d\n", i);
			// cudaError( cudaStreamDestroy(cudaDevices[i].stream) );
			cudaError( cuCtxDestroy(cudaDevices[i].context) );
		}
	}

	free(cudaDevices);

	// cuCtxDestroy(cudaContext);

//	cuCtxDestroy(cudaContext[0]);
//	cuCtxDestroy(cudaContext[1]);
//	cuCtxDestroy(cudaContext[2]);
//	cuCtxDestroy(cudaContext[3]);

//	for(i = cudaContext_count-1; i >0; i--)
//	{
		printf("cocotion pop current ctx\n");
		//cuError(cuCtxPopCurrent(&cudaContext[0])) ; //cocotion test
//		cuCtxDestroy(cudaContext[i]);
//	}
/*
	cuCtxDestroy(cudaContext);

	for(i = cudaContext_count-1; i >0; i--)
	{
		printf("cocotion pop current ctx\n");
		cuError(cuCtxPopCurrent(&cudaContext)) ; //cocotion test
		cuCtxDestroy(cudaContext);
	}
*/

}

void loadModuleKernels(int devId, void *fBin, char *fName,  uint32_t fId, uint32_t fNum)
{
	pfunc();
	ptrace("loading module.... fatBin= %16p ,name= '%s', fId = '%d'\n", fBin, fName, fId);
	// cuCtxSetCurrent(cudaDevices[devId].context);
	cuError( cuModuleLoadData( &cudaDevices[devId].module, fBin ));
	cuError( cuModuleGetFunction(&cudaDevices[devId].cudaFunction[fNum],
				cudaDevices[devId].module, fName) );
	cudaDevices[devId].cudaFunctionId[fNum] = fId;

	cudaDevices[devId].kernelsLoaded = 1;
}

void reloadAllKernels(void)
{
	pfunc();
	uint32_t i = 0;
	for( i = 0; i < cudaFunctionNum; i++ )
	{
		// void *fb = gpa_to_hva( devicesKernels[i].theArg.pA );
		// char *fn = gpa_to_hva( devicesKernels[i].theArg.pB );

		loadModuleKernels( cudaDeviceCurrent, devicesKernels[i].fatBin,
			devicesKernels[i].functionName, devicesKernels[i].funcId, i );
	}
}

static void qcu_cudaRegisterFunction(VirtioQCArg *arg)
{
	void *fatBin;
	char *functionName;
	uint32_t funcId;
	int i = 0;
	pfunc();

	// assume fatbin size is less equal 4MB
	fatBin       = gpa_to_hva(arg->pA);
	functionName = gpa_to_hva(arg->pB);
	funcId		 = arg->flag;

	devicesKernels[cudaFunctionNum].fatBin = fatBin;
	devicesKernels[cudaFunctionNum].funcId = funcId;
	devicesKernels[cudaFunctionNum].functionName = (char*) malloc( sizeof(char)*(strlen(functionName)+1) );
	strcpy( devicesKernels[cudaFunctionNum].functionName, functionName );
	// memcpy( &devicesKernels[cudaFunctionNum].theArg, arg, sizeof(VirtioQCArg) );

	ptrace("fatBin= %16p ,name= '%s', fId = '%d'\n", fatBin, functionName, funcId);
	// for(i = 0; i < totalDevices; i++)
	// {
	// 	cuError( cuCtxSetCurrent(cudaDevices[i].context) );
		// loadModuleKernels( i, fatBin, functionName, funcId, cudaFunctionNum );
		cuError( cuModuleLoadData( &cudaDevices[cudaDeviceCurrent].module, fatBin ) );
		cuError( cuModuleGetFunction(&cudaDevices[cudaDeviceCurrent].cudaFunction[cudaFunctionNum],
					cudaDevices[cudaDeviceCurrent].module, functionName) );
		cudaDevices[cudaDeviceCurrent].cudaFunctionId[cudaFunctionNum] = funcId;
	// }
	// cuError( cuCtxSetCurrent(cudaDevices[cudaDeviceCurrent].context) );

	cudaFunctionNum++;
}

static void qcu_cudaLaunch(VirtioQCArg *arg)
{
	//unsigned int *conf;
	uint64_t *conf;
	uint8_t *para;
	uint32_t funcId, paraNum, paraIdx, funcIdx;
	void **paraBuf;
	int i;
	pfunc();

	conf = gpa_to_hva(arg->pA);
	para = gpa_to_hva(arg->pB);
	paraNum = *((uint32_t*)para);
	funcId = arg->flag;

	ptrace("paraNum= %u\n", paraNum);

	paraBuf = malloc(paraNum*sizeof(void*));
	paraIdx = sizeof(uint32_t);

	for(i=0; i<paraNum; i++)
	{
		paraBuf[i] = &para[paraIdx+sizeof(uint32_t)];
		ptrace("arg %d = 0x%llx size= %u byte\n", i,
			*(unsigned long long*)paraBuf[i], *(unsigned int*)&para[paraIdx]);

		paraIdx += *((uint32_t*)&para[paraIdx]) + sizeof(uint32_t);
	}

	for(funcIdx=0; funcIdx<cudaFunctionNum; funcIdx++)
	{
		if( cudaDevices[cudaDeviceCurrent].cudaFunctionId[funcIdx] == funcId )
			break;
	}

	ptrace("grid (%u %u %u) block(%u %u %u) sharedMem(%u)\n",
			conf[0], conf[1], conf[2], conf[3], conf[4], conf[5], conf[6]);

//	cuError( cuLaunchKernel(cudaFunction[funcIdx],
//				conf[0], conf[1], conf[2],
//				conf[3], conf[4], conf[5],
//				conf[6], NULL, paraBuf, NULL)); // not suppoer stream yeat

// cudaDevices[cudaDeviceCurrent].stream

	cuError( cuLaunchKernel(cudaDevices[cudaDeviceCurrent].cudaFunction[funcIdx],
				conf[0], conf[1], conf[2],
				conf[3], conf[4], conf[5],
				conf[6], (conf[7]==(uint64_t)-1)?NULL:cudaStream[conf[7]], paraBuf, NULL)); // not suppoer stream yeat

	free(paraBuf);
}

////////////////////////////////////////////////////////////////////////////////
/// Memory Management (runtime API)
////////////////////////////////////////////////////////////////////////////////

static void qcu_cudaMalloc(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t count;
	void* devPtr;
	pfunc();

	count = arg->flag;
	cudaError((err = cudaMalloc( &devPtr, count )));
	arg->cmd = err;
	arg->pA = (uint64_t)devPtr;

	ptrace("ptr= %p ,count= %u\n", (void*)arg->pA, count);
}

static void qcu_cudaMemset(VirtioQCArg *arg)
{
	cudaError_t err;
	void* dst;

	dst = (void*)arg->pA;
	cudaError((err = cudaMemset(dst, arg->para, arg->pASize)));
	arg->cmd = err;
}

static void qcu_cudaMemcpy(VirtioQCArg *arg)
{
	//int fd;
	uint32_t size, len, i;

	cudaError_t err;
	void *dst, *src;
	uint64_t *gpa_array;

	pfunc();

	if( arg->flag == cudaMemcpyHostToDevice )
	{
		dst = (void*)arg->pA;
		size = arg->pBSize;
#ifdef USER_KERNEL_COPY
		if( size > QCU_KMALLOC_MAX_SIZE)
		{
#endif
   			if(arg->para)
			{
				src = gpa_to_hva(arg->pB);
				//src = gpa_to_hva(arg->rnd);
				err = cuMemcpyHtoD((CUdeviceptr)dst, src, size);
			}
			else
			{
				gpa_array = gpa_to_hva(arg->pB);

				uint32_t offset   	 = arg->pASize;
				uint32_t start_offset = offset%BLOCK_SIZE;
				uint32_t rsize = BLOCK_SIZE - start_offset;

				src = gpa_to_hva(gpa_array[0]);
            	len = MIN(size, rsize);
			//err = cudaMemcpy(dst, src, len, cudaMemcpyHostToDevice);
				err = cuMemcpyHtoD((CUdeviceptr)dst, src, len);

				size -= len;
				dst += len;

            	for(i=0; size>0; i++)
            	{
            		src = gpa_to_hva(gpa_array[i+1]);
                	len = MIN(size, BLOCK_SIZE);
				//err = cudaMemcpy(dst, src, len, cudaMemcpyHostToDevice);
					err = cuMemcpyHtoD((CUdeviceptr)dst, src, len);

					size -= len;
                	dst  += len;
            	}

			}

#ifdef USER_KERNEL_COPY
		}
		else
		{
			src = gpa_to_hva(arg->pB);
			//err = cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice);
			err = cuMemcpyHtoD((CUdeviceptr)dst, src, size);
		}
#endif
	}
	else if(arg->flag == cudaMemcpyDeviceToHost )
	{
		src = (void*)arg->pB;
		size = arg->pASize;

#ifdef USER_KERNEL_COPY
		if( size > QCU_KMALLOC_MAX_SIZE)
		{
#endif
			/*
			fd 	   = ldl_p(&arg->pBSize);
			offset = arg->pA;

    		dst = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
			err = cudaMemcpy(dst, src, size, cudaMemcpyDeviceToHost);
			*/

			if(arg->para)
			{
				dst = gpa_to_hva(arg->pA);
				//dst = gpa_to_hva(arg->rnd);
				err = cuMemcpyDtoH(dst, (CUdeviceptr)src, size);
			}
			else
			{
        		gpa_array = gpa_to_hva(arg->pA);

				uint32_t offset   	 = arg->pBSize;
				uint32_t start_offset = offset%BLOCK_SIZE;
				uint32_t rsize = BLOCK_SIZE - start_offset;

				dst = gpa_to_hva(gpa_array[0]);
            	len = MIN(size, rsize);
			//err = cudaMemcpy(dst, src, len, cudaMemcpyDeviceToHost);
				err = cuMemcpyDtoH(dst, (CUdeviceptr)src, len);

				size -= len;
				src+=len;

				for(i=0; size>0; i++)
  	            {
     		       	dst = gpa_to_hva(gpa_array[i+1]);
                	len = MIN(size, BLOCK_SIZE);
				//err = cudaMemcpy(dst, src, len, cudaMemcpyDeviceToHost);
					err = cuMemcpyDtoH(dst, (CUdeviceptr)src, len);

					size -= len;
              		src  += len;
            	}
			}
#ifdef USER_KERNEL_COPY
		}
		else
		{
			dst = gpa_to_hva(arg->pA);
			//err = cudaMemcpy(dst, src, size, cudaMemcpyDeviceToHost);
			err = cuMemcpyDtoH(dst, (CUdeviceptr)src, size);
		}
#endif
	}
	else if( arg->flag == cudaMemcpyDeviceToDevice )
	{
		dst = (void*)arg->pA;
		src = (void*)arg->pB;
		size = arg->pBSize;
		//cudaError(( err = cudaMemcpy(dst, src, size, cudaMemcpyDeviceToDevice)));
		cudaError(( err = cuMemcpyDtoD((CUdeviceptr)dst, (CUdeviceptr)src, size)));
	}

	arg->cmd = err;
	ptrace("size= %u\n", size);
}

static void qcu_cudaMemcpyAsync(VirtioQCArg *arg)
{
	int fd;
	uint64_t offset;
	uint32_t size;
	void *ptr, *device;
	cudaError_t err;
	//cudaStream_t stream = (cudaStream_t)arg->rnd;
	uint64_t streamIdx = arg->rnd;
	cudaStream_t stream = (streamIdx==(uint64_t)-1)?NULL:cudaStream[streamIdx];
  // cudaStream_t stream = (streamIdx==(uint64_t)-1)?cudaDevices[cudaDeviceCurrent].stream:cudaStream[streamIdx];

	if( arg->flag == cudaMemcpyHostToDevice )
	{
		fd = ldl_p(&arg->pASize);
		offset = arg->pB;
		size = arg->pBSize;

		device = (void*)arg->pA;

		//char *buf = (char*)malloc(sizeof(char)*size); //test
	//	pread(fd, buf, size, offset); //test

		//ptr = mmap(0, arg->para, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
		ptr = mmap(0, arg->para, PROT_READ, MAP_PRIVATE, fd, offset);
		err = cudaMemcpyAsync(device, ptr, size, cudaMemcpyHostToDevice, stream);

	//	err = cudaMemcpyAsync(device, buf, size, cudaMemcpyHostToDevice, stream); //text

	//	free(buf); //test
	}
	else if(arg->flag == cudaMemcpyDeviceToHost )
	{
		fd = ldl_p(&arg->pBSize);
		offset = arg->pA;
		size = arg->pASize;

		device = (void*)arg->pB;

	//	char *buf = (char*)malloc(sizeof(char)*size); //test

		//ptr = mmap(0, arg->para, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
		ptr = mmap(0, arg->para, PROT_WRITE, MAP_SHARED, fd, offset);
		msync(ptr, arg->para, MS_ASYNC);
		err = cudaMemcpyAsync(ptr, device, size, cudaMemcpyDeviceToHost, stream);

//		err = cudaMemcpyAsync(buf, device, size, cudaMemcpyDeviceToHost, stream); //test
//		pwrite(fd, buf, size, offset); //test
	//	free(buf); //test
	}
	else if( arg->flag == cudaMemcpyDeviceToDevice )
	{
		ptr = (void*)arg->pA;
		device = (void*)arg->pB;
		size = arg->pBSize;
		cudaError(( err = cudaMemcpyAsync(device, ptr, size, cudaMemcpyDeviceToDevice, stream)));
	}
/*
	uint32_t size, len, i;

	cudaError_t err;
	void *dst, *src;
	uint64_t *gpa_array;

	cudaStream_t stream = (cudaStream_t)arg->rnd;

	if( arg->flag == cudaMemcpyHostToDevice )
	{
		dst = (void*)arg->pA;
		size = arg->pBSize;

        gpa_array = gpa_to_hva(arg->pB);

		uint32_t offset   	 = arg->pASize;
		uint32_t start_offset = offset%BLOCK_SIZE;
		uint32_t rsize = BLOCK_SIZE - start_offset;

		src = gpa_to_hva(gpa_array[0]);
        len = MIN(size, rsize);
		err = cudaMemcpyAsync(dst, src, len, cudaMemcpyHostToDevice, stream);

		size -= len;
		dst += len;

		for(i=0; size>0; i++)
        {
        	src = gpa_to_hva(gpa_array[i+1]);
            len = MIN(size, BLOCK_SIZE);
			err = cudaMemcpyAsync(dst, src, len, cudaMemcpyHostToDevice, stream);

			size -= len;
            dst  += len;
        }
	}
	else if(arg->flag == cudaMemcpyDeviceToHost )
	{
		src = (void*)arg->pB;
		size = arg->pASize;

		gpa_array = gpa_to_hva(arg->pA);

		uint32_t offset   	 = arg->pBSize;
		uint32_t start_offset = offset%BLOCK_SIZE;
		uint32_t rsize = BLOCK_SIZE - start_offset;

		dst = gpa_to_hva(gpa_array[0]);
        len = MIN(size, rsize);
		err = cudaMemcpyAsync(dst, src, len, cudaMemcpyDeviceToHost, stream);

		size -= len;
		src+=len;

		for(i=0; size>0; i++)
        {
        	dst = gpa_to_hva(gpa_array[i+1]);
            len = MIN(size, BLOCK_SIZE);
			err = cudaMemcpyAsync(dst, src, len, cudaMemcpyDeviceToHost, stream);

			size -= len;
            src  += len;
        }
	}
	else if( arg->flag == cudaMemcpyDeviceToDevice )
	{
		dst = (void*)arg->pA;
		src = (void*)arg->pB;
		size = arg->pBSize;
		cudaError(( err = cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToDevice, stream)));
	}

	arg->cmd = err;
*/
	arg->cmd = err;
}


static void qcu_cudaFree(VirtioQCArg *arg)
{
	cudaError_t err;
	void* dst;
	pfunc();

	dst = (void*)arg->pA;
	cudaError((err = cudaFree(dst)));
	//cudaError((err = cuMemFree(dst)));
	arg->cmd = err;

	ptrace("ptr= %16p\n", dst);
}

////////////////////////////////////////////////////////////////////////////////
///	Device Management
////////////////////////////////////////////////////////////////////////////////

static void qcu_cudaGetDevice(VirtioQCArg *arg)
{
	cudaError_t err;
//	int device;
	pfunc();

	//cudaError((err = cudaGetDevice( &device )));
	err = 0;
	arg->cmd = err;
	//arg->pA = (uint64_t)device;
	arg->pA = (uint64_t)cudaDeviceCurrent;


	ptrace("device= %d\n", (int)cudaDeviceCurrent);
}

static void qcu_cudaGetDeviceCount(VirtioQCArg *arg)
{
	cudaError_t err;
	int device;
	pfunc();

	cudaError((err = cudaGetDeviceCount( &device )));
	arg->cmd = err;
	arg->pA = (uint64_t)device;

	ptrace("device count=%d\n", device);
}

static void qcu_cudaSetDevice(VirtioQCArg *arg)
{
	cudaError_t err;
	int device;
	pfunc();

	device = (int)arg->pA;
	cudaDeviceCurrent = device;

	if( device >= totalDevices )
	{
		arg->cmd = cudaErrorInvalidDevice;
		ptrace("error setting device= %d\n", device);
	} else {
		if( !memcmp( &zeroedDevice, &cudaDevices[device], sizeof(cudaDev) ) ) // device was reset therefore no context
		{
			printf("::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::\n\n");
			cuError( cuDeviceGet(&cudaDevices[device].device, device) );
			cuError( cuCtxCreate(&cudaDevices[device].context, 0, cudaDevices[device].device) );
		} else {
			// cuError( cuCtxPopCurrent(&cudaDevices[cudaDeviceCurrent].context) );
			// cuError( cuCtxPushCurrent(cudaDevices[device].context) );
			cuError( cuCtxSetCurrent(cudaDevices[device].context) );
			printf("***************************************************************\n");
			printf("cuda device %d\n", cudaDevices[device].device);
		}

		if( cudaDevices[cudaDeviceCurrent].kernelsLoaded == 0 )
			reloadAllKernels();

	//	cudaError((err = cudaSetDevice( device )));

	//	cuError(cuCtxPushCurrent(cudaContext));	//now test

	//	cuError(cuCtxPopCurrent(&cudaContext)) ; //cocotion test

		// cuError( cuDeviceGet(&cudaDeviceCurrent, device) );// cocotion test
	//	cuError( cuCtxCreate(&cudaContext, 0, cudaDevice) ); //cocotion test

		cudaContext_count++;
		//cuError(cuCtxPopCurrent(&cudaContext)) ; //cocotion test


	//	cuError(cuCtxPushCurrent(cudaContext)); //now test

		printf("cocotion in cudaSetDevice\n");
		err = 0; //cocotion test

		arg->cmd = err;

		ptrace("set device= %d\n", device);
	}
}

static void qcu_cudaGetDeviceProperties(VirtioQCArg *arg)
{
	cudaError_t err;
	struct cudaDeviceProp *prop;
	int device;
	pfunc();

	prop = gpa_to_hva(arg->pA);
	device = (int)arg->pB;

	cudaError((err = cudaGetDeviceProperties( prop, device )));
	arg->cmd = err;

	ptrace("get prop for device %d\n", device);
}

static void qcu_cudaDeviceSynchronize(VirtioQCArg *arg)
{
	cudaError_t err;
	pfunc();
	cudaError((err = cudaDeviceSynchronize()));
	//err = cuCtxSynchronize(); //cocotion test
	arg->cmd = err;
}

static void qcu_cudaDeviceReset(VirtioQCArg *arg)
{
	cudaError_t err;
	pfunc();
	cudaError((err = cudaDeviceReset()));
	memset( &cudaDevices[cudaDeviceCurrent], 0, sizeof(cudaDev) );
	arg->cmd = err;
}

////////////////////////////////////////////////////////////////////////////////
///	Version Management
////////////////////////////////////////////////////////////////////////////////

static void qcu_cudaDriverGetVersion(VirtioQCArg *arg)
{
	cudaError_t err;
	int version;
	pfunc();

	cudaError((err = cudaDriverGetVersion( &version )));
	arg->cmd = err;
	arg->pA = (uint64_t)version;

	ptrace("driver version= %d\n", version);
}

static void qcu_cudaRuntimeGetVersion(VirtioQCArg *arg)
{
	cudaError_t err;
	int version;
	pfunc();

	cudaError((err = cudaRuntimeGetVersion( &version )));
	arg->cmd = err;
	arg->pA = (uint64_t)version;

	ptrace("runtime driver= %d\n", version);
}

//////////////////////////////////////////////////
//static void qcu_checkCudaCapabilities(VirtioQCArg *arg)
//{
//	cudaError_t err;
//	err = checkCudaCapabilities(arg->pA, arg->pB);
//	arg->cmd = err;
//}

//////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
///	Event Management
////////////////////////////////////////////////////////////////////////////////

static void qcu_cudaEventCreate(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t idx;
	pfunc();

	idx = cudaEventNum;
	cudaError((err = cudaEventCreate(&cudaEvent[idx])));
	arg->cmd = err;
	arg->pA = (uint64_t)idx;

	cudaEventNum++;
	ptrace("create event %u\n", idx);
}

static void qcu_cudaEventCreateWithFlags(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t idx;

	idx = cudaEventNum;
	cudaError((err = cudaEventCreateWithFlags(&cudaEvent[idx], arg->flag)));
	arg->cmd = err;
	arg->pA = (uint64_t)idx;

	cudaEventNum++;
	ptrace("create event %u\n", idx);
}

static void qcu_cudaEventRecord(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t eventIdx;
//	uint32_t streamIdx;
	uint64_t streamIdx;
	pfunc();

	eventIdx  = arg->pA;
	streamIdx = arg->pB;
//	cudaError((err = cudaEventRecord(cudaEvent[eventIdx], cudaStream[streamIdx])));
	cudaError((err = cudaEventRecord(cudaEvent[eventIdx], (streamIdx==(uint64_t)-1)?NULL:cudaStream[streamIdx]  )));
	// cudaError((err = cudaEventRecord(cudaEvent[eventIdx], (streamIdx==(uint64_t)-1)?cudaDevices[cudaDeviceCurrent].stream:cudaStream[streamIdx]  )));

	arg->cmd = err;

	ptrace("event record %u\n", eventIdx);
}

static void qcu_cudaEventSynchronize(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t idx;
	pfunc();

	idx = arg->pA;
	cudaError((err = cudaEventSynchronize( cudaEvent[idx] )));
	arg->cmd = err;

	ptrace("sync event %u\n", idx);
}

static void qcu_cudaEventElapsedTime(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t startIdx;
	uint32_t endIdx;
	float ms;
	pfunc();

	startIdx = arg->pA;
	endIdx   = arg->pB;
	cudaError((err = cudaEventElapsedTime(&ms, cudaEvent[startIdx], cudaEvent[endIdx])));
	arg->cmd = err;
	memcpy(&arg->flag, &ms, sizeof(float));

	ptrace("event elapse time= %f, start= %u, end= %u\n",
			ms, startIdx, endIdx);
}

static void qcu_cudaEventDestroy(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t idx;
	pfunc();

	idx = arg->pA;
	cudaError((err = cudaEventDestroy(cudaEvent[idx])));
	arg->cmd = err;
	memset(&cudaEvent[idx], 0, sizeof(cudaEvent_t));

	ptrace("destroy event %u\n", idx);
}

////////////////////////////////////////////////////////////////////////////////
///	Error Handling
////////////////////////////////////////////////////////////////////////////////

static void qcu_cudaGetLastError(VirtioQCArg *arg)
{
	cudaError_t err;
	pfunc();

	err =  cudaGetLastError();
	arg->cmd = err;
	ptrace("lasr cudaError %d\n", err);
}

//////////zero-copy////////

static void qcu_cudaHostRegister(VirtioQCArg *arg)
{
	int fd = ldl_p(&arg->pBSize);
	uint64_t offset = arg->pA;
	uint32_t size = arg->pASize;

	void *ptr = mmap(0, arg->pB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);

	cudaError_t err;
	err = cudaHostRegister(ptr, size, arg->flag);
	//err = cuMemHostRegister(ptr, size, CU_MEMHOSTREGISTER_DEVICEMAP);
	arg->pA = (uint64_t)ptr;
	arg->cmd = err;
}

static void qcu_cudaHostGetDevicePointer(VirtioQCArg *arg)
{
	cudaError_t err;
	void *ptr = (void*)	arg->pB;
	void *devPtr;
	//CUdeviceptr devPtr;
	err = cudaHostGetDevicePointer(&devPtr, ptr, arg->flag);
	//err = cuMemHostGetDevicePointer(&devPtr, ptr, arg->flag);
	arg->pA = (uint64_t)devPtr;
	arg->cmd = err;
}

static void qcu_cudaHostUnregister(VirtioQCArg *arg)
{
	void *ptr = (void*) arg->pB;

	cudaError_t err;
	err = cudaHostUnregister(ptr);
	//err = cuMemHostUnregister(ptr);
	arg->cmd = err;
}

static void qcu_cudaSetDeviceFlags(VirtioQCArg *arg)
{
	cudaError_t err;
//	err = cudaSetDeviceFlags(arg->flag);
	err = 0;
	arg->cmd = err;
}

//stream
static void qcu_cudaStreamCreate(VirtioQCArg *arg)
{
	cudaError_t err;
//	void *ptr = (void*) arg->pA;
//	err = cuStreamCreate((CUstream*)ptr,0);

	err = cudaStreamCreate(&cudaStream[cudaStreamNum]);
	arg->pA = cudaStreamNum;
	cudaStreamNum++;
 	arg->cmd = err;


}

static void qcu_cudaStreamDestroy(VirtioQCArg *arg)
{
	cudaError_t err;
	uint32_t idx;

	idx = arg->pA;
	cudaError((err = cudaStreamDestroy(cudaStream[idx])));
	arg->cmd = err;
	memset(&cudaStream[idx], 0, sizeof(cudaStream_t));
}

#endif // CONFIG_CUDA

static int qcu_cmd_write(VirtioQCArg *arg)
{
	void   *src, *dst;
	uint64_t *gpa_array;
	uint32_t size, len, i;

	size = arg->pASize;

	ptrace("szie= %u\n", size);

	if(deviceSpace!=NULL)
	{
		free(deviceSpace);
	}

	deviceSpaceSize = size;
	deviceSpace = (char*)malloc(deviceSpaceSize);

	if( size > deviceSpaceSize )
	{
		gpa_array = gpa_to_hva(arg->pA);
		dst = deviceSpace;
		for(i=0; size>0; i++)
		{
			len = MIN(size, QCU_KMALLOC_MAX_SIZE);
			src = gpa_to_hva(gpa_array[i]);
			memcpy(dst, src, len);
			size -= len;
			dst  += len;
		}
	}
	else
	{
		src = gpa_to_hva(arg->pA);
		memcpy(deviceSpace, src, size);
	}
	// checker ------------------------------------------------------------
/*
	uint64_t err;
	if( deviceSpaceSize<32 )
	{
		for(i=0; i<deviceSpaceSize; i++)
		{
			ptrace("deviceSpace[%lu]= %d\n", i, deviceSpace[i]);
		}
	}
	else
	{
		err = 0;
		for(i=0; i<deviceSpaceSize; i++)
		{
			if( deviceSpace[i] != (i%17)*7 ) err++;
		}
		ptrace("error= %llu\n", (unsigned long long)err);
	}
	ptrace("\n\n");
	//---------------------------------------------------------------------
*/
	return 0;
}

static int qcu_cmd_read(VirtioQCArg *arg)
{
	void   *src, *dst;
	uint64_t *gpa_array;
	uint32_t size, len, i;

	if(deviceSpace==NULL)
	{
		return -1;
	}

	size = arg->pASize;

	ptrace("szie= %u\n", size);

	if( size > deviceSpaceSize )
	{
		gpa_array = gpa_to_hva(arg->pA);
		src = deviceSpace;
		for(i=0; size>0; i++)
		{
			len = MIN(size, QCU_KMALLOC_MAX_SIZE);
			dst = gpa_to_hva(gpa_array[i]);
			memcpy(dst, src, len);
			size -= len;
			src  += len;
		}
	}
	else
	{
		dst = gpa_to_hva(arg->pA);
		memcpy(dst, deviceSpace, size);
	}

	return 0;
}

static int qcu_cmd_mmapctl(VirtioQCArg *arg)
{

	int pid = getpid();
	char vfname[100];

	sprintf(vfname, "/home/coldfunction/qcuvf/vm%d_%lx", pid, arg->pB);

	int fd=open(vfname, O_CREAT|O_RDWR,0666);
    if(fd<0)
    {
        printf("failure to open\n");
        exit(0);
    }
	if(lseek(fd,arg->pBSize,SEEK_SET)==-1) //cocotion modified
    {
        printf("Failure to lseek\n");
        exit(0);
	}
    if(write(fd, "",1) != 1)
    {
        printf("Failure on write\n");
        exit(0);
    }

	stl_p(&arg->pA, fd);

	return 0;
}

static int qcu_cmd_open(VirtioQCArg *arg)
{
	BLOCK_SIZE = arg->pASize;
/*
	int fd=open(VIRTHM_DEV_PATH, O_CREAT|O_RDWR,0666);
    if(fd<0)
    {
        printf("failure to open\n");
        exit(0);
    }
	if(lseek(fd,MAP_MAX_LENGTH-1,SEEK_SET)==-1) //cocotion modified
    {
        printf("Failure to lseek\n");
        exit(0);
	}
    if(write(fd, "",1) != 1)
    {
        printf("Failure on write\n");
        exit(0);
    }
	stl_p(&arg->pA, fd);

	printf("cocotion open fd: %d in host\n", fd);
*/
	//arg->pA = getpid();
	//printf("pid: %d in host\n", arg->pA);

	//buf = (char*)malloc(sizeof(int)*16*1024*1024); //test
	return 0;

}

static int qcu_cmd_close(VirtioQCArg *arg)
{
	//free(buf); //cocotion test
/*
	int fd = ldl_p(&arg->pA);
	printf("cocotion close fd: %d in close\n", fd);
	int resp = close(fd);
	stl_p(&arg->pB, resp);
*/
	return 0;
}

static int qcu_cmd_mmap(VirtioQCArg *arg){

	//arg->pA: file fd
	//arg->pASize: numOfblocks
	//arg->pB: gpa_array

	int32_t fd = (int32_t)ldl_p(&arg->pA);
	uint64_t *gpa_array = gpa_to_hva(arg->pB);
	void *addr;

	int i;
	for(i = 0; i < arg->pASize; i++)
	{
		addr = gpa_to_hva(gpa_array[i]);
		munmap(addr, BLOCK_SIZE);
   		mmap(addr, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, i*BLOCK_SIZE);
	}


    return 0;
}

static int qcu_cmd_munmap(VirtioQCArg *arg){
   	// arg->pB: gpa_array
	// arg->pBSize: numOfblocks

	uint64_t *gpa_array = gpa_to_hva(arg->pB);
	void *addr;

	int i;
	for(i = 0; i < arg->pBSize; i++)
	{
		addr    = gpa_to_hva(gpa_array[i]);
		munmap(addr, BLOCK_SIZE);
		mmap(addr, BLOCK_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
   	}

	return 0;

}

static int qcu_cmd_mmaprelease(VirtioQCArg *arg)
{
	close(ldl_p(&arg->pBSize));

	//TODO: safely check

	int pid = getpid();
	char vfname[100];
	sprintf(vfname, "/home/coldfunction/qcuvf/vm%d_%lx", pid, arg->pA);

	unlink(vfname);

	return 0;
}

static void virtio_qcuda_cmd_handle(VirtIODevice *vdev, VirtQueue *vq)
{
	VirtQueueElement elem;
	VirtioQCArg *arg;

	arg = malloc( sizeof(VirtioQCArg));
	while( virtqueue_pop(vq, &elem) )
	{
		iov_to_buf(elem.out_sg, elem.out_num, 0, arg, sizeof(VirtioQCArg));

		switch( arg->cmd )
		{
			case VIRTQC_CMD_WRITE:
				qcu_cmd_write(arg);
				break;

			case VIRTQC_CMD_READ:
				qcu_cmd_read(arg);
				break;

			case VIRTQC_CMD_OPEN:
				qcu_cmd_open(arg);
				break;

			case VIRTQC_CMD_CLOSE:
				qcu_cmd_close(arg);
				break;

			case VIRTQC_CMD_MMAP:
				qcu_cmd_mmap(arg);
				break;

			case VIRTQC_CMD_MUNMAP:
				qcu_cmd_munmap(arg);
				break;

			case VIRTQC_CMD_MMAPCTL:
				qcu_cmd_mmapctl(arg);
				break;

			case VIRTQC_CMD_MMAPRELEASE:
				qcu_cmd_mmaprelease(arg);
				break;

#ifdef CONFIG_CUDA
			// Module & Execution control (driver API)
			case VIRTQC_cudaRegisterFatBinary:
				qcu_cudaRegisterFatBinary(arg);
				break;

			case VIRTQC_cudaUnregisterFatBinary:
				qcu_cudaUnregisterFatBinary(arg);
				break;

			case VIRTQC_cudaRegisterFunction:
				qcu_cudaRegisterFunction(arg);
				break;

			case VIRTQC_cudaLaunch:
				qcu_cudaLaunch(arg);
				break;

			// Memory Management (runtime API)
			case VIRTQC_cudaMalloc:
				qcu_cudaMalloc(arg);
				break;

			case VIRTQC_cudaMemset:
				qcu_cudaMemset(arg);
				break;

			case VIRTQC_cudaMemcpy:
				qcu_cudaMemcpy(arg);
				break;

			case VIRTQC_cudaMemcpyAsync:
				qcu_cudaMemcpyAsync(arg);
				break;

			case VIRTQC_cudaFree:
				qcu_cudaFree(arg);
				break;

			// Device Management (runtime API)
			case VIRTQC_cudaGetDevice:
				qcu_cudaGetDevice(arg);
				break;

			case VIRTQC_cudaGetDeviceCount:
				qcu_cudaGetDeviceCount(arg);
				break;

			case VIRTQC_cudaSetDevice:
				qcu_cudaSetDevice(arg);
				break;

			case VIRTQC_cudaGetDeviceProperties:
				qcu_cudaGetDeviceProperties(arg);
				break;

			case VIRTQC_cudaDeviceSynchronize:
				qcu_cudaDeviceSynchronize(arg);
				break;

			case VIRTQC_cudaDeviceReset:
				qcu_cudaDeviceReset(arg);
				break;

			// Version Management (runtime API)
			case VIRTQC_cudaDriverGetVersion:
				qcu_cudaDriverGetVersion(arg);
				break;

			case VIRTQC_cudaRuntimeGetVersion:
				qcu_cudaRuntimeGetVersion(arg);
				break;
///////////////////////////////////////////////
		//	case VIRTQC_checkCudaCapabilities:
		//		qcu_checkCudaCapabilities(arg);
///////////////////////////////////////////////

			//stream
			case VIRTQC_cudaStreamCreate:
				qcu_cudaStreamCreate(arg);
				break;

			case VIRTQC_cudaStreamDestroy:
				qcu_cudaStreamDestroy(arg);
				break;

			// Event Management (runtime API)
			case VIRTQC_cudaEventCreate:
				qcu_cudaEventCreate(arg);
				break;

			case VIRTQC_cudaEventCreateWithFlags:
				qcu_cudaEventCreateWithFlags(arg);
				break;

			case VIRTQC_cudaEventRecord:
				qcu_cudaEventRecord(arg);
				break;

			case VIRTQC_cudaEventSynchronize:
				qcu_cudaEventSynchronize(arg);
				break;

			case VIRTQC_cudaEventElapsedTime:
				qcu_cudaEventElapsedTime(arg);
				break;

			case VIRTQC_cudaEventDestroy:
				qcu_cudaEventDestroy(arg);
				break;

			// Error Handling (runtime API)
			case VIRTQC_cudaGetLastError:
				qcu_cudaGetLastError(arg);
				break;

			//zero-copy
			case VIRTQC_cudaHostRegister:
				qcu_cudaHostRegister(arg);
				break;

			case VIRTQC_cudaHostGetDevicePointer:
				qcu_cudaHostGetDevicePointer(arg);
				break;

			case VIRTQC_cudaHostUnregister:
				qcu_cudaHostUnregister(arg);
				break;

			case VIRTQC_cudaSetDeviceFlags:
				qcu_cudaSetDeviceFlags(arg);
				break;

			//case VIRTQC_cudaFreeHost:
			//	qcu_cudaFreeHost(arg);
			//	break;

#endif
			default:
				error("unknow cmd= %d\n", arg->cmd);
		}

		iov_from_buf(elem.in_sg, elem.in_num, 0, arg, sizeof(VirtioQCArg));
		virtqueue_push(vq, &elem, sizeof(VirtioQCArg));
		virtio_notify(vdev, vq);
	}
		free(arg);
}

//####################################################################
//   class basic callback functions
//####################################################################

static void virtio_qcuda_device_realize(DeviceState *dev, Error **errp)
{
	VirtIODevice *vdev = VIRTIO_DEVICE(dev);
	VirtIOQC *qcu = VIRTIO_QC(dev);
	//Error *err = NULL;

	//ptrace("GPU mem size=%"PRIu64"\n", qcu->conf.mem_size);

	virtio_init(vdev, "virtio-qcuda", VIRTIO_ID_QC, sizeof(VirtIOQCConf));

	qcu->vq  = virtio_add_queue(vdev, 1024, virtio_qcuda_cmd_handle);
}

static uint64_t virtio_qcuda_get_features(VirtIODevice *vdev, uint64_t features, Error **errp)
{
	//ptrace("feature=%"PRIu64"\n", features);
	return features;
}

/*
   static void virtio_qcuda_device_unrealize(DeviceState *dev, Error **errp)
   {
   ptrace("\n");
   }

   static void virtio_qcuda_get_config(VirtIODevice *vdev, uint8_t *config)
   {
   ptrace("\n");
   }

   static void virtio_qcuda_set_config(VirtIODevice *vdev, const uint8_t *config)
   {
   ptrace("\n");
   }

   static void virtio_qcuda_reset(VirtIODevice *vdev)
   {
   ptrace("\n");
   }

   static void virtio_qcuda_save_device(VirtIODevice *vdev, QEMUFile *f)
   {
   ptrace("\n");
   }

   static int virtio_qcuda_load_device(VirtIODevice *vdev, QEMUFile *f, int version_id)
   {
   ptrace("\n");
   return 0;
   }

   static void virtio_qcuda_set_status(VirtIODevice *vdev, uint8_t status)
   {
   ptrace("\n");
   }
 */

/*
   get the configure
ex: -device virtio-qcuda,size=2G,.....
DEFINE_PROP_SIZE(config name, device struce, element, default value)
 */
static Property virtio_qcuda_properties[] =
{
	DEFINE_PROP_SIZE("size", VirtIOQC, conf.mem_size, 0),
	DEFINE_PROP_END_OF_LIST(),
};

static void virtio_qcuda_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

	dc->props = virtio_qcuda_properties;

	set_bit(DEVICE_CATEGORY_MISC, dc->categories);

	vdc->get_features = virtio_qcuda_get_features;

	vdc->realize = virtio_qcuda_device_realize;
	/*
		vdc->unrealize = virtio_qcuda_device_unrealize;

		vdc->get_config = virtio_qcuda_get_config;
		vdc->set_config = virtio_qcuda_set_config;

		vdc->save = virtio_qcuda_save_device;
		vdc->load = virtio_qcuda_load_device;

		vdc->set_status = virtio_qcuda_set_status;
		vdc->reset = virtio_qcuda_reset;
	 */
}

static void virtio_qcuda_instance_init(Object *obj)
{
}

static const TypeInfo virtio_qcuda_device_info = {
	.name = TYPE_VIRTIO_QC,
	.parent = TYPE_VIRTIO_DEVICE,
	.instance_size = sizeof(VirtIOQC),
	.instance_init = virtio_qcuda_instance_init,
	.class_init = virtio_qcuda_class_init,
};

static void virtio_qcuda_register_types(void)
{
	type_register_static(&virtio_qcuda_device_info);
}

type_init(virtio_qcuda_register_types)
