AMDGPU Acronym List
===================

This is a non-exhaustive list of the acronyms I've seen in ROCm code, including
the AMDGPU driver. Mostly, I just remember to update this list after I've
needed to spend time tracking down the definition of non-obvious acronyms. So,
I won't include obvious things like "AMD".

 - BO: Buffer Object
 - CP: Command Processor
 - CPSCH: Command Processor Scheduling (used when HWS is enabled)
 - DIQ: Debug Interface Queue. A queue for the kernel like the HIQ, but for sending debugging commands (`drivers/gpu/drm/amd/include/kgd_kfd_interface.h`)
 - GART: Graphics address remapping table. Used to map noncontiguous physical memory for GPU DMA. [See this](https://en.wikipedia.org/wiki/Graphics_address_remapping_table)
 - GEM: Graphics Execution Manager. Intended to be a simpler alternative in response to TTM. Not sure if this is relevant to my work? See the TTM article.
 - GTT: Graphics Translation Table. Another term for GART, at least according to wikipedia. In the driver, "GTT memory" refers to system memory that can be accessed by the GPU.
 - GWS: Global Wave Sync, memory (?) through which all waves on the GPU can synchronize. [LLVM docs.](https://www.kernel.org/doc/html/latest/gpu/amdgpu.html)
 - HIQ: HSA Interface Queue. Special queue for the kernel to send commands to the GPU (`drivers/gpu/drm/amd/include/kgd_kfd_interface.h`)
 - HQD: Hardware Queue Descriptor.  A place for holding MQDs in GPU hardware.  There are a limited number of "slots."
 - HWS: Hardware Scheduling, a scheduling policy using the CP
 - IB: Indirect Buffer, "areas of GPU-accessible memory where commands are stored" (`drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c`)
 - MQD: Memory Queue Descriptor.  A GPU-accessible CPU-side data structure holding information about a queue.
 - NOCPSCH: No Command Processor Scheduling (used when HWS is disabled)
 - PASID: A global address space identifier that can be shared between the GPU, IOMMU, and the driver. (See `drivers/gpu/drm/amd/amdgpu/amdgpu_ids.c`)
 - QCM: Queue Control Management, see patch discussion [here](https://lwn.net/Articles/607730/)
 - SGPR: Scalar General Purpose Register. See the LLVM documentation.
 - SPI: Shader Pipe Interface.  Mentioned in the patent [here](https://patentimages.storage.googleapis.com/03/03/e7/5d1b1c95b8a79e/US20210049729A1.pdf).
 - TTM: Translation Table Maps. Used by the linux kernel to manage graphics memory. See [this.](https://www.kernel.org/doc/html/v4.10/gpu/drm-mm.html)

