/* White-box wrapper fault seams, real Vulkan beneath them. Include upstream
 * first so failures affect our ownership code, not VkFFT internals. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <vulkan/vulkan.h>
#include <glslang_c_interface.h>
#include <vkFFT/vkFFT.h>
#include "check.h"
#include <string.h>

enum fault { NONE, CREATE, ALLOCATE, BIND, MAP, SUBMIT, WAIT, TIMEOUT, BEGIN, END, RESET };
static enum fault fault;
static int countdown, hit, buffers, memories, mappings, outstanding;

static int inject(enum fault point)
{
	if (fault != point || --countdown != 0) return 0;
	fault = NONE;
	hit++;
	return 1;
}

static VkResult create(VkDevice d, const VkBufferCreateInfo *i, const VkAllocationCallbacks *a, VkBuffer *b)
{
	VkResult r;
	if (inject(CREATE)) return VK_ERROR_OUT_OF_HOST_MEMORY;
	r = vkCreateBuffer(d, i, a, b);
	if (r == VK_SUCCESS) buffers++;
	return r;
}
static void destroy(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a)
{
	CHECK(!outstanding && buffers > 0);
	buffers--;
	vkDestroyBuffer(d, b, a);
}
static VkResult allocate(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
	VkResult r;
	if (inject(ALLOCATE)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	r = vkAllocateMemory(d, i, a, m);
	if (r == VK_SUCCESS) memories++;
	return r;
}
static void release(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a)
{
	CHECK(!outstanding && memories > 0);
	memories--;
	vkFreeMemory(d, m, a);
}
static VkResult bind(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o)
{
	if (inject(BIND)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	return vkBindBufferMemory(d, b, m, o);
}
static VkResult map(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void **p)
{
	VkResult r;
	if (inject(MAP)) return VK_ERROR_MEMORY_MAP_FAILED;
	r = vkMapMemory(d, m, o, s, f, p);
	if (r == VK_SUCCESS) mappings++;
	return r;
}
static void unmap(VkDevice d, VkDeviceMemory m)
{
	CHECK(!outstanding && mappings > 0);
	mappings--;
	vkUnmapMemory(d, m);
}
static VkResult submit(VkQueue q, uint32_t c, const VkSubmitInfo *i, VkFence f)
{
	VkResult r;
	CHECK(!outstanding);
	if (inject(SUBMIT)) return VK_ERROR_OUT_OF_HOST_MEMORY;
	r = vkQueueSubmit(q, c, i, f);
	if (r == VK_SUCCESS) outstanding = 1;
	return r;
}
static VkResult wait_for(VkDevice d, uint32_t c, const VkFence *f, VkBool32 all, uint64_t timeout)
{
	VkResult r;
	CHECK(outstanding && timeout == UINT64_MAX);
	if (inject(WAIT)) return VK_ERROR_OUT_OF_HOST_MEMORY;
	if (inject(TIMEOUT)) return VK_TIMEOUT;
	r = vkWaitForFences(d, c, f, all, timeout);
	if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) outstanding = 0;
	return r;
}
static VkResult idle(VkDevice d)
{
	VkResult r = vkDeviceWaitIdle(d);
	if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) outstanding = 0;
	return r;
}
static VkResult begin(VkCommandBuffer c, const VkCommandBufferBeginInfo *i)
{
	if (inject(BEGIN)) return VK_ERROR_OUT_OF_HOST_MEMORY;
	return vkBeginCommandBuffer(c, i);
}
static VkResult end(VkCommandBuffer c)
{
	if (inject(END)) return VK_ERROR_OUT_OF_HOST_MEMORY;
	return vkEndCommandBuffer(c);
}
static VkResult reset(VkCommandBuffer c, VkCommandBufferResetFlags f)
{
	CHECK(!outstanding);
	if (inject(RESET)) return VK_ERROR_OUT_OF_HOST_MEMORY;
	return vkResetCommandBuffer(c, f);
}
#define vkCreateBuffer create
#define vkDestroyBuffer destroy
#define vkAllocateMemory allocate
#define vkFreeMemory release
#define vkBindBufferMemory bind
#define vkMapMemory map
#define vkUnmapMemory unmap
#define vkQueueSubmit submit
#define vkWaitForFences wait_for
#define vkDeviceWaitIdle idle
#define vkBeginCommandBuffer begin
#define vkEndCommandBuffer end
#define vkResetCommandBuffer reset
#include "../src/fft/fft_vkfft.c"

static void clean(void)
{
	CHECK(buffers == 0 && memories == 0 && mappings == 0 && outstanding == 0);
}

int main(void)
{
	float in[32] = {1}, output[32], saved[32];
	const enum fault setup[] = {CREATE, ALLOCATE, BIND, MAP};
	const enum fault execution[] = {SUBMIT, WAIT, TIMEOUT, BEGIN, END, RESET};
	for (size_t i = 0; i < sizeof(setup)/sizeof(*setup); i++) {
		for (int nth = 1; nth <= (setup[i] == MAP ? 1 : 2); nth++) {
			fault = setup[i]; countdown = nth; hit = 0;
			CHECK(!fft_plan_create(16));
			CHECK(hit == 1);
			clean();
		}
	}
	for (size_t i = 0; i < sizeof(execution)/sizeof(*execution); i++) {
		for (int inplace = 0; inplace < 2; inplace++) {
			struct fft_plan *p = fft_plan_create(16);
			CHECK(p);
			memcpy(output, in, sizeof(in)); memcpy(saved, output, sizeof(output));
			fault = execution[i]; countdown = 1; hit = 0;
			CHECK(fft_execute(p, inplace ? output : in, output) < 0);
			CHECK(hit == 1 && p->failed && !p->pending && !outstanding);
			CHECK(memcmp(output, saved, sizeof(output)) == 0);
			CHECK(fft_execute(p, in, output) < 0);
			CHECK(memcmp(output, saved, sizeof(output)) == 0);
			fft_plan_destroy(p);
			clean();
		}
	}
	puts("VkFFT wrapper: 7 setup and 12 execution faults passed; no pending release");
	return 0;
}
