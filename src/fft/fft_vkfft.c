#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <locale.h>
#include <pthread.h>
#include <time.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include <vulkan/vulkan.h>
#include <glslang_c_interface.h>
#include <vkFFT/vkFFT.h>
#include "fft_backend.h"

struct fft_plan {
	int nfft, failed, pending, app_initialized;
	float *window;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkFence fence;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkBuffer buffer, staging;
	VkDeviceMemory memory, staging_memory;
	void *mapped;
	/* VkFFT retains these addresses, including the launch command address. */
	pfUINT bytes;
	VkFFTApplication app;
};

/* Process lifetime ownership: never finalize glslang out from under other users.
 * Plan compilation is serialized; locale changes affect only this thread. */
static pthread_once_t compiler_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t compiler_mutex = PTHREAD_MUTEX_INITIALIZER;
static int compiler_ready;

static void compiler_init(void)
{
	compiler_ready = glslang_initialize_process();
}

static int memory_type(VkPhysicalDevice physical, uint32_t bits,
	VkMemoryPropertyFlags flags, uint32_t *index)
{
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(physical, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
		if ((bits & (1u << i)) &&
		    (properties.memoryTypes[i].propertyFlags & flags) == flags) {
			*index = i;
			return 0;
		}
	}
	return -1;
}

/* Ownership transfers only on full success; failure leaves both outputs zero. */
static int create_buffer(struct fft_plan *p, VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags, VkBuffer *out, VkDeviceMemory *out_memory)
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = p->bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	if (vkCreateBuffer(p->device, &ci, NULL, &buffer) != VK_SUCCESS)
		return -1;
	vkGetBufferMemoryRequirements(p->device, buffer, &requirements);
	ai.allocationSize = requirements.size;
	if (memory_type(p->physical, requirements.memoryTypeBits, flags,
		&ai.memoryTypeIndex) < 0)
		goto fail;
	if (vkAllocateMemory(p->device, &ai, NULL, &memory) != VK_SUCCESS) {
		memory = VK_NULL_HANDLE; /* An unsuccessful call transfers no ownership. */
		goto fail;
	}
	if (vkBindBufferMemory(p->device, buffer, memory, 0) != VK_SUCCESS)
		goto fail;
	*out = buffer;
	*out_memory = memory;
	return 0;
fail:
	vkDestroyBuffer(p->device, buffer, NULL);
	if (memory)
		vkFreeMemory(p->device, memory, NULL);
	return -1;
}

static int extension_present(VkPhysicalDevice physical, const char *name)
{
	uint32_t count = 0;
	VkExtensionProperties *extensions;
	VkResult result;
	int found = 0;
	result = physical ? vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) :
		vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
	if (result != VK_SUCCESS)
		return -1;
	extensions = calloc(count ? count : 1, sizeof(*extensions));
	if (!extensions)
		return -1;
	result = physical ? vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions) :
		vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);
	if (result == VK_SUCCESS) {
		for (uint32_t i = 0; i < count; i++)
			if (!strcmp(extensions[i].extensionName, name))
				found = 1;
	} else {
		found = -1;
	}
	free(extensions);
	return found;
}

static int create_device(struct fft_plan *p, VkPhysicalDevice physical, uint32_t *family)
{
	uint32_t count = 0, memory_index;
	VkQueueFamilyProperties *properties;
	VkDevice device;
	float priority = 1;
	const char *portability = "VK_KHR_portability_subset";
	int portable = extension_present(physical, portability), result = -1;
	VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
	if (portable < 0 || memory_type(physical, UINT32_MAX,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&memory_index) < 0 || memory_type(physical, UINT32_MAX,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_index) < 0)
		return -1;
	if (portable) {
		ci.enabledExtensionCount = 1;
		ci.ppEnabledExtensionNames = &portability;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
	properties = calloc(count ? count : 1, sizeof(*properties));
	if (!properties)
		return -1;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, properties);
	for (uint32_t i = 0; i < count; i++) {
		if (!properties[i].queueCount || !(properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
			continue;
		qi.queueFamilyIndex = i;
		if (vkCreateDevice(physical, &ci, NULL, &device) == VK_SUCCESS) {
			p->device = device;
			p->physical = physical;
			*family = i;
			vkGetDeviceQueue(p->device, i, 0, &p->queue);
			result = 0;
			break;
		}
	}
	free(properties);
	return result;
}

/* A timeout is NOT cancellation. Never release resources still in use.
 * Other wait errors poison the plan but require an idle/lost-device result
 * before cleanup. This can wait indefinitely on a broken driver. */
static int complete_submission(struct fft_plan *p)
{
	VkResult result;
	if (!p->pending)
		return 0;
	result = vkWaitForFences(p->device, 1, &p->fence, VK_TRUE, UINT64_MAX);
	if (result != VK_SUCCESS) {
		p->failed = 1;
		if (result != VK_ERROR_DEVICE_LOST) {
			do {
				const struct timespec pause = {0, 1000000};
				result = vkDeviceWaitIdle(p->device);
				if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
					nanosleep(&pause, NULL);
			} while (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST);
		}
	}
	p->pending = 0;
	return p->failed ? -1 : 0;
}

struct fft_plan *fft_plan_create(int nfft)
{
	struct fft_plan *p;
	uint32_t count = 0, family = 0;
	VkPhysicalDevice *devices = NULL;
	const char *portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
	const char *instance_extensions[] = { VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
	int portable;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "rtl_stream", .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkFFTConfiguration config = {0};
	VkFFTResult initialized;
	locale_t locale, previous;
	if (nfft < 2 || nfft > 16384 || (nfft & (nfft - 1)))
		return NULL;
	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->nfft = nfft;
	p->bytes = (pfUINT)nfft * 2 * sizeof(float);
	p->window = malloc((size_t)nfft * sizeof(float));
	if (!p->window)
		goto fail;
	for (int i = 0; i < nfft; i++) {
		double x = 2 * M_PI * i / (nfft - 1);
		p->window[i] = (float)(0.35875 - 0.48829*cos(x) + 0.14128*cos(2*x) - 0.01168*cos(3*x));
	}
	portable = extension_present(VK_NULL_HANDLE, portability);
	if (portable < 0)
		goto fail;
	if (portable) {
		/* portability_subset depends on this instance extension in Vulkan 1.0. */
		if (extension_present(VK_NULL_HANDLE, instance_extensions[1]) != 1)
			goto fail;
		ii.enabledExtensionCount = 2;
		ii.ppEnabledExtensionNames = instance_extensions;
		ii.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}
	if (vkCreateInstance(&ii, NULL, &p->instance) != VK_SUCCESS) {
		p->instance = VK_NULL_HANDLE;
		goto fail;
	}
	if (vkEnumeratePhysicalDevices(p->instance, &count, NULL) != VK_SUCCESS || !count)
		goto fail;
	devices = calloc(count, sizeof(*devices));
	if (!devices || vkEnumeratePhysicalDevices(p->instance, &count, devices) != VK_SUCCESS)
		goto fail;
	for (uint32_t i = 0; i < count; i++)
		if (create_device(p, devices[i], &family) == 0)
			break;
	free(devices);
	devices = NULL;
	if (!p->device)
		goto fail;
	pi.queueFamilyIndex = family;
	if (vkCreateFence(p->device, &fi, NULL, &p->fence) != VK_SUCCESS) {
		p->fence = VK_NULL_HANDLE;
		goto fail;
	}
	if (vkCreateCommandPool(p->device, &pi, NULL, &p->pool) != VK_SUCCESS) {
		p->pool = VK_NULL_HANDLE;
		goto fail;
	}
	ai.commandPool = p->pool;
	if (vkAllocateCommandBuffers(p->device, &ai, &p->command) != VK_SUCCESS) {
		p->command = VK_NULL_HANDLE;
		goto fail;
	}
	if (create_buffer(p, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		&p->buffer, &p->memory) < 0 ||
	    create_buffer(p, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&p->staging, &p->staging_memory) < 0)
		goto fail;
	if (vkMapMemory(p->device, p->staging_memory, 0, p->bytes, 0, &p->mapped) != VK_SUCCESS) {
		p->mapped = NULL;
		goto fail;
	}
	config.FFTdim = 1;
	config.size[0] = (pfUINT)nfft;
	config.makeForwardPlanOnly = 1;
	/* Precomputed twiddles avoid device sin/cos accuracy differences, especially
	 * near zero components of large complex tones. See upstream timeout caveat
	 * in docs/rtl_stream.md: LUT uploads occur inside initializeVkFFT. */
	config.useLUT = 1;
	config.physicalDevice = &p->physical;
	config.device = &p->device;
	config.queue = &p->queue;
	config.commandPool = &p->pool;
	config.fence = &p->fence;
	config.buffer = &p->buffer;
	config.bufferSize = &p->bytes;
	config.isCompilerInitialized = 1;
	config.disableSetLocale = 1;
	locale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
	if (!locale)
		goto fail;
	previous = uselocale(locale);
	if (!previous) {
		freelocale(locale);
		goto fail;
	}
	pthread_once(&compiler_once, compiler_init);
	pthread_mutex_lock(&compiler_mutex);
	initialized = compiler_ready ? initializeVkFFT(&p->app, config) : VKFFT_ERROR_FAILED_TO_INITIALIZE;
	pthread_mutex_unlock(&compiler_mutex);
	uselocale(previous);
	freelocale(locale);
	if (initialized != VKFFT_SUCCESS) {
		fprintf(stderr, "VkFFT initialization failed: %d\n", (int)initialized);
		goto fail;
	}
	p->app_initialized = 1;
	return p;
fail:
	free(devices);
	fft_plan_destroy(p);
	return NULL;
}

void fft_plan_destroy(struct fft_plan *p)
{
	if (!p)
		return;
	complete_submission(p);
	if (p->app_initialized)
		deleteVkFFT(&p->app);
	if (p->device) {
		if (p->mapped)
			vkUnmapMemory(p->device, p->staging_memory);
		if (p->buffer)
			vkDestroyBuffer(p->device, p->buffer, NULL);
		if (p->memory)
			vkFreeMemory(p->device, p->memory, NULL);
		if (p->staging)
			vkDestroyBuffer(p->device, p->staging, NULL);
		if (p->staging_memory)
			vkFreeMemory(p->device, p->staging_memory, NULL);
		if (p->pool)
			vkDestroyCommandPool(p->device, p->pool, NULL);
		if (p->fence)
			vkDestroyFence(p->device, p->fence, NULL);
		vkDestroyDevice(p->device, NULL);
	}
	if (p->instance)
		vkDestroyInstance(p->instance, NULL);
	free(p->window);
	free(p);
}

static void barrier(struct fft_plan *p, VkBuffer buffer, VkPipelineStageFlags src,
	VkAccessFlags read, VkPipelineStageFlags dst, VkAccessFlags write)
{
	VkBufferMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = read, .dstAccessMask = write,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = buffer, .size = p->bytes };
	vkCmdPipelineBarrier(p->command, src, dst, 0, 0, NULL, 1, &b, 0, NULL);
}

int fft_execute(struct fft_plan *p, const float *in, float *out)
{
	uintptr_t a = (uintptr_t)in, b = (uintptr_t)out;
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkBufferCopy copy = {0};
	VkFFTLaunchParams launch = {0};
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1 };
	if (!p || !in || !out || p->failed)
		return -1;
	if (a != b && (a > b ? a - b : b - a) < p->bytes)
		return -1;
	if (vkResetCommandBuffer(p->command, 0) != VK_SUCCESS ||
	    vkResetFences(p->device, 1, &p->fence) != VK_SUCCESS ||
	    vkBeginCommandBuffer(p->command, &bi) != VK_SUCCESS)
		goto fail;
	memcpy(p->mapped, in, (size_t)p->bytes);
	copy.size = p->bytes;
	vkCmdCopyBuffer(p->command, p->staging, p->buffer, 1, &copy);
	barrier(p, p->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	launch.commandBuffer = &p->command;
	launch.buffer = &p->buffer;
	if (VkFFTAppend(&p->app, -1, &launch) != VKFFT_SUCCESS)
		goto fail;
	barrier(p, p->buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	vkCmdCopyBuffer(p->command, p->buffer, p->staging, 1, &copy);
	barrier(p, p->staging, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
	if (vkEndCommandBuffer(p->command) != VK_SUCCESS)
		goto fail;
	si.pCommandBuffers = &p->command;
	if (vkQueueSubmit(p->queue, 1, &si, p->fence) != VK_SUCCESS)
		goto fail;
	p->pending = 1;
	if (complete_submission(p) < 0)
		goto fail;
	memcpy(out, p->mapped, (size_t)p->bytes);
	return 0;
fail:
	p->failed = 1;
	return -1;
}

float *fft_get_window(struct fft_plan *p)
{
	return p ? p->window : NULL;
}

const char *fft_backend_name(void)
{
	return "Vulkan (VkFFT)"; /* Immutable: multiple live plans never race here. */
}
