#ifdef HAVE_VKFFT

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "vulkan/vulkan.h"
#include "glslang_c_interface.h"
#include "vkFFT/vkFFT.h"
#include "fft_backend.h"

#define FFT_MAX_NFFT 16384

static double bh_coeff(int n, int N)
{
	double a0 = 0.35875;
	double a1 = 0.48829;
	double a2 = 0.14128;
	double a3 = 0.01168;
	double x = 2.0 * M_PI * n / (N - 1);
	return a0 - a1 * cos(x) + a2 * cos(2.0 * x) - a3 * cos(3.0 * x);
}

struct fft_plan {
	int nfft;
	float *window;

	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue queue;
	uint32_t queueFamilyIndex;
	VkFence fence;
	VkCommandPool commandPool;

	VkBuffer buffer;
	VkDeviceMemory bufferMemory;
	uint64_t bufferSize;

	VkBuffer stagingBuffer;
	VkDeviceMemory stagingBufferMemory;
	uint64_t stagingBufferSize;

	VkFFTApplication app;
	int appInitialized;
	int glslangInitialized;
};

static int find_queue_family(VkPhysicalDevice phys, VkQueueFlags flags)
{
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, NULL);
	if (!count)
		return -1;
	VkQueueFamilyProperties *props = (VkQueueFamilyProperties *)malloc(count * sizeof(*props));
	if (!props)
		return -1;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props);
	int idx = -1;
	for (uint32_t i = 0; i < count; i++) {
		if (props[i].queueCount > 0 && (props[i].queueFlags & flags)) {
			idx = (int)i;
			break;
		}
	}
	free(props);
	return idx;
}

static int find_memory_type(VkPhysicalDevice phys, uint32_t typeBits,
			    VkMemoryPropertyFlags props, uint32_t *outIdx)
{
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
	for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
		if ((typeBits & (1u << i)) &&
		    (memProps.memoryTypes[i].propertyFlags & props) == props) {
			*outIdx = i;
			return 0;
		}
	}
	return -1;
}

static int create_buffer(VkDevice dev, VkPhysicalDevice phys,
			 VkDeviceSize size, VkBufferUsageFlags usage,
			 VkMemoryPropertyFlags memProps,
			 VkBuffer *outBuf, VkDeviceMemory *outMem)
{
	VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ci.size = size;
	ci.usage = usage;
	ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(dev, &ci, NULL, outBuf) != VK_SUCCESS)
		return -1;

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, *outBuf, &req);
	uint32_t memType;
	if (find_memory_type(phys, req.memoryTypeBits, memProps, &memType) < 0) {
		vkDestroyBuffer(dev, *outBuf, NULL);
		return -1;
	}
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = memType;
	if (vkAllocateMemory(dev, &ai, NULL, outMem) != VK_SUCCESS) {
		vkDestroyBuffer(dev, *outBuf, NULL);
		return -1;
	}
	vkBindBufferMemory(dev, *outBuf, *outMem, 0);
	return 0;
}

static int copy_buffer(VkDevice dev, VkQueue queue, VkCommandPool pool,
		       VkFence fence, VkBuffer src, VkBuffer dst,
		       VkDeviceSize size)
{
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS)
		return -1;

	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	VkBufferCopy region;
	region.srcOffset = 0;
	region.dstOffset = 0;
	region.size = size;
	vkCmdCopyBuffer(cmd, src, dst, 1, &region);
	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VkResult res = vkQueueSubmit(queue, 1, &si, fence);
	if (res != VK_SUCCESS) {
		vkFreeCommandBuffers(dev, pool, 1, &cmd);
		return -1;
	}
	vkWaitForFences(dev, 1, &fence, VK_TRUE, 100000000000ULL);
	vkResetFences(dev, 1, &fence);
	vkFreeCommandBuffers(dev, pool, 1, &cmd);
	return 0;
}

struct fft_plan *fft_plan_create(int nfft)
{
	struct fft_plan *plan;
	VkResult res;
	int i;

	if (nfft < 2 || nfft > FFT_MAX_NFFT)
		return NULL;

	plan = (struct fft_plan *)calloc(1, sizeof(*plan));
	if (!plan)
		return NULL;

	plan->nfft = nfft;
	plan->window = (float *)malloc(nfft * sizeof(float));
	if (!plan->window)
		goto fail;
	for (i = 0; i < nfft; i++)
		plan->window[i] = (float)bh_coeff(i, nfft);

	plan->bufferSize = (uint64_t)nfft * 2 * sizeof(float);
	plan->stagingBufferSize = plan->bufferSize;

	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "rtl_stream";
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = "rtl_stream";
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo instCi = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instCi.pApplicationInfo = &appInfo;
	instCi.enabledLayerCount = 0;
	res = vkCreateInstance(&instCi, NULL, &plan->instance);
	if (res != VK_SUCCESS)
		goto fail;

	uint32_t physCount = 0;
	res = vkEnumeratePhysicalDevices(plan->instance, &physCount, NULL);
	if (res != VK_SUCCESS || physCount == 0)
		goto fail;
	VkPhysicalDevice *physDevs = (VkPhysicalDevice *)malloc(physCount * sizeof(VkPhysicalDevice));
	if (!physDevs)
		goto fail;
	vkEnumeratePhysicalDevices(plan->instance, &physCount, physDevs);
	plan->physicalDevice = physDevs[0];
	free(physDevs);

	int qfi = find_queue_family(plan->physicalDevice, VK_QUEUE_COMPUTE_BIT);
	if (qfi < 0)
		goto fail;
	plan->queueFamilyIndex = (uint32_t)qfi;

	float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo qCi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qCi.queueFamilyIndex = plan->queueFamilyIndex;
	qCi.queueCount = 1;
	qCi.pQueuePriorities = &queuePriority;

	VkDeviceCreateInfo devCi = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	devCi.queueCreateInfoCount = 1;
	devCi.pQueueCreateInfos = &qCi;
	res = vkCreateDevice(plan->physicalDevice, &devCi, NULL, &plan->device);
	if (res != VK_SUCCESS)
		goto fail;

	vkGetDeviceQueue(plan->device, plan->queueFamilyIndex, 0, &plan->queue);

	VkFenceCreateInfo fCi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	res = vkCreateFence(plan->device, &fCi, NULL, &plan->fence);
	if (res != VK_SUCCESS)
		goto fail;

	VkCommandPoolCreateInfo cpCi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpCi.queueFamilyIndex = plan->queueFamilyIndex;
	res = vkCreateCommandPool(plan->device, &cpCi, NULL, &plan->commandPool);
	if (res != VK_SUCCESS)
		goto fail;

	VkBufferUsageFlags bufUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (create_buffer(plan->device, plan->physicalDevice, plan->bufferSize,
			  bufUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			  &plan->buffer, &plan->bufferMemory) < 0)
		goto fail;
	if (create_buffer(plan->device, plan->physicalDevice, plan->stagingBufferSize,
			  bufUsage,
			  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			  &plan->stagingBuffer, &plan->stagingBufferMemory) < 0)
		goto fail;

	if (!plan->glslangInitialized) {
		plan->glslangInitialized = 1;
		glslang_initialize_process();
	}

	VkFFTConfiguration cfg = {0};
	cfg.FFTdim = 1;
	cfg.size[0] = (pfUINT)nfft;
	cfg.numberBatches = 1;
	cfg.makeForwardPlanOnly = 1;
	cfg.physicalDevice = &plan->physicalDevice;
	cfg.device = &plan->device;
	cfg.queue = &plan->queue;
	cfg.commandPool = &plan->commandPool;
	cfg.fence = &plan->fence;
	cfg.isCompilerInitialized = 1;
	cfg.buffer = &plan->buffer;
	cfg.bufferSize = &plan->bufferSize;

	memset(&plan->app, 0, sizeof(plan->app));
	if (initializeVkFFT(&plan->app, cfg) != VKFFT_SUCCESS)
		goto fail;
	plan->appInitialized = 1;

	return plan;

fail:
	fft_plan_destroy(plan);
	return NULL;
}

void fft_plan_destroy(struct fft_plan *plan)
{
	if (!plan)
		return;

	if (plan->appInitialized)
		deleteVkFFT(&plan->app);

	if (plan->device) {
		if (plan->buffer)
			vkDestroyBuffer(plan->device, plan->buffer, NULL);
		if (plan->bufferMemory)
			vkFreeMemory(plan->device, plan->bufferMemory, NULL);
		if (plan->stagingBuffer)
			vkDestroyBuffer(plan->device, plan->stagingBuffer, NULL);
		if (plan->stagingBufferMemory)
			vkFreeMemory(plan->device, plan->stagingBufferMemory, NULL);
		if (plan->commandPool)
			vkDestroyCommandPool(plan->device, plan->commandPool, NULL);
		if (plan->fence)
			vkDestroyFence(plan->device, plan->fence, NULL);
		vkDestroyDevice(plan->device, NULL);
	}
	if (plan->instance)
		vkDestroyInstance(plan->instance, NULL);

	if (plan->glslangInitialized)
		glslang_finalize_process();

	free(plan->window);
	free(plan);
}

static void apply_window(struct fft_plan *plan, const float *in)
{
	int n = plan->nfft;
	void *data;
	if (vkMapMemory(plan->device, plan->stagingBufferMemory, 0,
			plan->stagingBufferSize, 0, &data) != VK_SUCCESS)
		return;
	float *dst = (float *)data;
	for (int i = 0; i < n; i++) {
		dst[2 * i]     = in[2 * i]     * plan->window[i];
		dst[2 * i + 1] = in[2 * i + 1] * plan->window[i];
	}
	vkUnmapMemory(plan->device, plan->stagingBufferMemory);
}

void fft_execute(struct fft_plan *plan, const float *in, float *out)
{
	int n = plan->nfft;

	apply_window(plan, in);

	if (copy_buffer(plan->device, plan->queue, plan->commandPool, plan->fence,
			plan->stagingBuffer, plan->buffer, plan->bufferSize) < 0)
		return;

	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = plan->commandPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	if (vkAllocateCommandBuffers(plan->device, &ai, &cmd) != VK_SUCCESS)
		return;

	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	VkFFTLaunchParams lp = {0};
	lp.commandBuffer = &cmd;
	lp.buffer = &plan->buffer;

	if (VkFFTAppend(&plan->app, -1, &lp) != VKFFT_SUCCESS) {
		vkFreeCommandBuffers(plan->device, plan->commandPool, 1, &cmd);
		return;
	}

	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	if (vkQueueSubmit(plan->queue, 1, &si, plan->fence) == VK_SUCCESS) {
		vkWaitForFences(plan->device, 1, &plan->fence, VK_TRUE, 100000000000ULL);
		vkResetFences(plan->device, 1, &plan->fence);
	}
	vkFreeCommandBuffers(plan->device, plan->commandPool, 1, &cmd);

	if (copy_buffer(plan->device, plan->queue, plan->commandPool, plan->fence,
			plan->buffer, plan->stagingBuffer, plan->bufferSize) < 0)
		return;

	void *data;
	if (vkMapMemory(plan->device, plan->stagingBufferMemory, 0,
			plan->stagingBufferSize, 0, &data) == VK_SUCCESS) {
		memcpy(out, data, plan->stagingBufferSize);
		vkUnmapMemory(plan->device, plan->stagingBufferMemory);
	}
}

float *fft_get_window(struct fft_plan *plan)
{
	return plan ? plan->window : NULL;
}

#else
#include "fft_backend.h"
/* stub: VkFFT not available */
struct fft_plan *fft_plan_create(int nfft) { (void)nfft; return NULL; }
void fft_plan_destroy(struct fft_plan *plan) { (void)plan; }
void fft_execute(struct fft_plan *plan, const float *in, float *out)
{ (void)plan; (void)in; (void)out; }
float *fft_get_window(struct fft_plan *plan) { (void)plan; return NULL; }
#endif
