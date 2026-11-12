std::uint32_t VkDeviceR::memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(physical_, &props);
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  return 0xFFFFFFFFu;
}

VkBuffer VkDeviceR::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags mem,
                                VkDeviceMemory* out_mem, void** map) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  vkCreateBuffer(device_, &bi, nullptr, &buffer);
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device_, buffer, &req);
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = memory_type(req.memoryTypeBits, mem);
  if (flags.flags) ai.pNext = &flags;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  vkAllocateMemory(device_, &ai, nullptr, &memory);
  vkBindBufferMemory(device_, buffer, memory, 0);
  if (map && (mem & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) vkMapMemory(device_, memory, 0, size, 0, map);
  if (out_mem) *out_mem = memory;
  return buffer;
}

void VkDeviceR::destroy_buffer_raw(VkBuffer b, VkDeviceMemory m) {
  if (b) vkDestroyBuffer(device_, b, nullptr);
  if (m) vkFreeMemory(device_, m, nullptr);
}

void VkDeviceR::one_shot(const std::function<void(VkCommandBuffer)>& fn) {
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer c = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device_, &ai, &c);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(c, &bi);
  fn(c);
  vkEndCommandBuffer(c);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &c;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue_);
  vkFreeCommandBuffers(device_, pool_, 1, &c);
}

void VkDeviceR::transition(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, bool depth) {
  VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  bar.oldLayout = from;
  bar.newLayout = to;
  bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bar.image = image;
  bar.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  bar.subresourceRange.levelCount = 1;
  bar.subresourceRange.layerCount = 1;
  bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  bar.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &bar);
}

VkSampler VkDeviceR::sampler_of(const SamplerDesc& d) {
  return (d.wrap == Wrap::Clamp) ? linear_clamp_ : linear_wrap_;
}

VkDescriptorSet VkDeviceR::write_set(VkCmd& c, bool ray) {
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = desc_pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = ray ? &rt_layout_ : &set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkDescriptorBufferInfo bufs[3]{};
  VkDescriptorImageInfo imgs[12]{};
  VkWriteDescriptorSet writes[16]{};
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    bufs[i].buffer = c.ubo[i].buffer ? c.ubo[i].buffer : dummy_->buffer;
    bufs[i].offset = c.ubo[i].buffer ? c.ubo[i].offset : 0;
    bufs[i].range = c.ubo[i].buffer ? std::max<VkDeviceSize>(c.ubo[i].size, 16) : dummy_->size;
    writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[n].dstSet = set;
    writes[n].dstBinding = static_cast<uint32_t>(i);
    writes[n].descriptorCount = 1;
    writes[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[n].pBufferInfo = &bufs[i];
    ++n;
  }
  for (int i = 0; i < 12; ++i) {
    const int slot = i + 3;
    VkImageView view = c.tex[slot].view;
    if (!view) view = (slot == 6 && far_depth_) ? far_depth_->view : (slot == 4 && flat_ ? flat_->view : white_->view);
    imgs[i].sampler = c.tex[slot].sampler ? c.tex[slot].sampler : linear_wrap_;
    imgs[i].imageView = view;
    imgs[i].imageLayout = (slot == 6) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[n].dstSet = set;
    writes[n].dstBinding = static_cast<uint32_t>(slot);
    writes[n].descriptorCount = 1;
    writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[n].pImageInfo = &imgs[i];
    ++n;
  }
  VkWriteDescriptorSetAccelerationStructureKHR as_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  if (ray && c.tlas) {
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &c.tlas;
    writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[n].dstSet = set;
    writes[n].dstBinding = 15;
    writes[n].descriptorCount = 1;
    writes[n].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[n].pNext = &as_write;
    ++n;
  }
  vkUpdateDescriptorSets(device_, static_cast<uint32_t>(n), writes, 0, nullptr);
  return set;
}

bool VkDeviceR::open(const DeviceDesc& desc, std::string* error) {
  desc_ = desc;
  static bool volk_ready = false;
  if (!volk_ready) {
    if (volkInitialize() != VK_SUCCESS) {
      if (error) *error = "Vulkan loader not found (libvulkan / vulkan-1.dll)";
      return false;
    }
    volk_ready = true;
  }
  std::uint32_t ext_count = 0;
  const char** glfw_ext = nullptr;
  if (!desc.offscreen) glfw_ext = glfwGetRequiredInstanceExtensions(&ext_count);
  std::vector<const char*> exts(glfw_ext, glfw_ext + ext_count);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Nidavellir";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  ici.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();
  if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
    if (error) *error = "vkCreateInstance failed";
    return false;
  }
  volkLoadInstance(instance_);
  std::uint32_t ndev = 0;
  vkEnumeratePhysicalDevices(instance_, &ndev, nullptr);
  if (!ndev) {
    if (error) *error = "no Vulkan device";
    return false;
  }
  std::vector<VkPhysicalDevice> devs(ndev);
  vkEnumeratePhysicalDevices(instance_, &ndev, devs.data());
  physical_ = devs[0];
  for (VkPhysicalDevice d : devs) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(d, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { physical_ = d; break; }
  }
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical_, &props);
  name_ = props.deviceName;
  set_uniform_align(props.limits.minUniformBufferOffsetAlignment);

  if (!desc.offscreen) {
    if (!desc.window) {
      if (!glfwInit()) {
        if (error) *error = "glfwInit failed";
        return false;
      }
      owned_glfw_ = true;
      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
      window_ = glfwCreateWindow(desc.width, desc.height, desc.title ? desc.title : "Nidavellir", nullptr, nullptr);
      owned_window_ = true;
    } else window_ = static_cast<GLFWwindow*>(desc.window);
    if (!window_ || glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
      if (error) *error = "Vulkan surface failed";
      return false;
    }
  }
  std::uint32_t nfam = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nfam, nullptr);
  std::vector<VkQueueFamilyProperties> fams(nfam);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nfam, fams.data());
  family_ = 0;
  for (std::uint32_t i = 0; i < nfam; ++i) {
    if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
    if (surface_) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(physical_, i, surface_, &present);
      if (!present) continue;
    }
    family_ = i;
    break;
  }

  raytracing_ = false;
  VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  asf.pNext = &bda;
  rqf.pNext = &asf;
  VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &rqf};
  vkGetPhysicalDeviceFeatures2(physical_, &feats);
  std::vector<const char*> dext;
  if (surface_) dext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  if (rqf.rayQuery && asf.accelerationStructure && bda.bufferDeviceAddress) {
    dext.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    dext.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    dext.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    raytracing_ = true;
  }
  float prio = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceBufferDeviceAddressFeatures bda_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR rq_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  if (raytracing_) {
    bda_on.bufferDeviceAddress = VK_TRUE;
    as_on.accelerationStructure = VK_TRUE;
    rq_on.rayQuery = VK_TRUE;
    as_on.pNext = &bda_on;
    rq_on.pNext = &as_on;
    enable.pNext = &rq_on;
  }
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = raytracing_ ? &enable : nullptr;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(dext.size());
  dci.ppEnabledExtensionNames = dext.empty() ? nullptr : dext.data();
  if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
    if (error) *error = "vkCreateDevice failed";
    return false;
  }
  volkLoadDevice(device_);
  vkGetDeviceQueue(device_, family_, 0, &queue_);

  color_format_ = VK_FORMAT_R8G8B8A8_UNORM;
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = family_;
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  vkCreateCommandPool(device_, &pci, nullptr, &pool_);
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = pool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  vkAllocateCommandBuffers(device_, &cbai, &command_);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  vkCreateFence(device_, &fci, nullptr, &fence_);

  auto make_pass = [&](bool color, bool depth, VkFormat cf, VkImageLayout final_color, bool present) {
    VkAttachmentDescription att[2]{};
    int n = 0;
    VkAttachmentReference cref{};
    VkAttachmentReference dref{};
    if (color) {
      att[n].format = cf;
      att[n].samples = VK_SAMPLE_COUNT_1_BIT;
      att[n].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      att[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      att[n].finalLayout = final_color;
      cref.attachment = static_cast<uint32_t>(n);
      cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ++n;
    }
    if (depth) {
      att[n].format = VK_FORMAT_D32_SFLOAT;
      att[n].samples = VK_SAMPLE_COUNT_1_BIT;
      att[n].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      att[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      att[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      dref.attachment = static_cast<uint32_t>(n);
      dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      ++n;
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = color ? 1 : 0;
    sub.pColorAttachments = color ? &cref : nullptr;
    sub.pDepthStencilAttachment = depth ? &dref : nullptr;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = static_cast<uint32_t>(n);
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    (void)present;
    VkRenderPass pass = VK_NULL_HANDLE;
    vkCreateRenderPass(device_, &rp, nullptr, &pass);
    return pass;
  };
  color_pass_ = make_pass(true, true, color_format_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
  depth_pass_ = make_pass(false, true, color_format_, VK_IMAGE_LAYOUT_UNDEFINED, false);

  auto sampler = [&](VkSamplerAddressMode wrap) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = wrap;
    si.maxLod = 16;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(device_, &si, nullptr, &s);
    return s;
  };
  linear_wrap_ = sampler(VK_SAMPLER_ADDRESS_MODE_REPEAT);
  linear_clamp_ = sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

  std::vector<VkDescriptorSetLayoutBinding> binds(12);
  for (int i = 0; i < 3; ++i) {
    binds[i].binding = static_cast<uint32_t>(i);
    binds[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[i].descriptorCount = 1;
    binds[i].stageFlags = VK_SHADER_STAGE_ALL;
  }
  for (int i = 3; i < 15; ++i) {
    if (static_cast<int>(binds.size()) < i + 1) binds.resize(i + 1);
    binds[i].binding = static_cast<uint32_t>(i);
    binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[i].descriptorCount = 1;
    binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sl.bindingCount = 15;
  sl.pBindings = binds.data();
  vkCreateDescriptorSetLayout(device_, &sl, nullptr, &set_layout_);
  if (raytracing_) {
    binds.push_back({});
    binds.back().binding = 15;
    binds.back().descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    binds.back().descriptorCount = 1;
    binds.back().stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    sl.bindingCount = static_cast<uint32_t>(binds.size());
    sl.pBindings = binds.data();
    vkCreateDescriptorSetLayout(device_, &sl, nullptr, &rt_layout_);
  }
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &set_layout_;
  vkCreatePipelineLayout(device_, &pl, nullptr, &pipe_layout_);
  if (rt_layout_) {
    pl.pSetLayouts = &rt_layout_;
    vkCreatePipelineLayout(device_, &pl, nullptr, &rt_pipe_layout_);
  }
  VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3000},
                                   {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4000},
                                   {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 200}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1000;
  dpi.poolSizeCount = raytracing_ ? 3u : 2u;
  dpi.pPoolSizes = sizes;
  vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_);

  BufferDesc dd;
  dd.kind = BufferKind::Uniform;
  dd.size = 256;
  dummy_ = static_cast<VkBufferR*>(create_buffer(dd, nullptr));
  TextureDesc td;
  td.width = td.height = 1;
  unsigned char white[4] = {255, 255, 255, 255};
  unsigned char flat[4] = {128, 128, 255, 255};
  white_ = static_cast<VkTextureR*>(create_texture(td, white));
  flat_ = static_cast<VkTextureR*>(create_texture(td, flat));
  td.format = TextureFormat::Depth;
  far_depth_ = static_cast<VkTextureR*>(create_texture(td, nullptr));

  if (surface_) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
    std::uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &nf, formats.data());
    VkSurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats) {
      if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = f; break; }
    }
    swap_format_ = fmt.format;
    swap_pass_ = make_pass(true, false, swap_format_, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
      extent.width = static_cast<uint32_t>(desc.width);
      extent.height = static_cast<uint32_t>(desc.height);
    }
    width_ = static_cast<int>(extent.width);
    height_ = static_cast<int>(extent.height);
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(2u, caps.minImageCount);
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    vkCreateSwapchainKHR(device_, &sci, nullptr, &swap_);
    std::uint32_t ni = 0;
    vkGetSwapchainImagesKHR(device_, swap_, &ni, nullptr);
    std::vector<VkImage> images(ni);
    vkGetSwapchainImagesKHR(device_, swap_, &ni, images.data());
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = swap_format_;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    for (VkImage image : images) {
      vi.image = image;
      VkImageView view = VK_NULL_HANDLE;
      vkCreateImageView(device_, &vi, nullptr, &view);
      swap_views_.push_back(view);
      VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fi.renderPass = swap_pass_;
      fi.attachmentCount = 1;
      fi.pAttachments = &swap_views_.back();
      fi.width = extent.width;
      fi.height = extent.height;
      fi.layers = 1;
      VkFramebuffer fb = VK_NULL_HANDLE;
      vkCreateFramebuffer(device_, &fi, nullptr, &fb);
      swap_fbs_.push_back(fb);
    }
    VkFenceCreateInfo af{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device_, &af, nullptr, &acquire_fence_);
    image_count_ = ni;
  } else {
    width_ = desc.width;
    height_ = desc.height;
  }
  cmd_.dev = this;
  cmd_.cmd = command_;
  return true;
}

void VkDeviceR::shutdown() {
  if (device_) vkDeviceWaitIdle(device_);
  rt_reset();
  destroy_texture(white_);
  destroy_texture(flat_);
  destroy_texture(far_depth_);
  destroy_buffer(dummy_);
  white_ = flat_ = far_depth_ = nullptr;
  dummy_ = nullptr;
  for (VkFramebuffer fb : swap_fbs_) vkDestroyFramebuffer(device_, fb, nullptr);
  for (VkImageView view : swap_views_) vkDestroyImageView(device_, view, nullptr);
  swap_fbs_.clear();
  swap_views_.clear();
  if (acquire_fence_) vkDestroyFence(device_, acquire_fence_, nullptr);
  if (swap_) vkDestroySwapchainKHR(device_, swap_, nullptr);
  if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
  if (pipe_layout_) vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
  if (rt_pipe_layout_) vkDestroyPipelineLayout(device_, rt_pipe_layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  if (rt_layout_) vkDestroyDescriptorSetLayout(device_, rt_layout_, nullptr);
  if (linear_wrap_) vkDestroySampler(device_, linear_wrap_, nullptr);
  if (linear_clamp_) vkDestroySampler(device_, linear_clamp_, nullptr);
  if (color_pass_) vkDestroyRenderPass(device_, color_pass_, nullptr);
  if (depth_pass_) vkDestroyRenderPass(device_, depth_pass_, nullptr);
  if (swap_pass_) vkDestroyRenderPass(device_, swap_pass_, nullptr);
  if (fence_) vkDestroyFence(device_, fence_, nullptr);
  if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
  if (device_) vkDestroyDevice(device_, nullptr);
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
  if (owned_window_ && window_) glfwDestroyWindow(window_);
  if (owned_glfw_) glfwTerminate();
  device_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
  window_ = nullptr;
}

Buffer* VkDeviceR::create_buffer(const BufferDesc& desc, const void* data) {
  auto* b = new VkBufferR();
  b->kind = desc.kind;
  b->size = std::max<VkDeviceSize>(desc.size, 16);
  VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (desc.kind == BufferKind::Vertex) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  else if (desc.kind == BufferKind::Index) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  else if (desc.kind == BufferKind::Uniform) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  else usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (raytracing_ && (desc.kind == BufferKind::Vertex || desc.kind == BufferKind::Index)) {
    usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  }
  b->buffer = make_buffer(b->size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &b->memory, &b->map);
  if (data && b->map) std::memcpy(b->map, data, desc.size);
  return b;
}
void VkDeviceR::update_buffer(Buffer* buffer, const void* data, std::size_t size, std::size_t offset) {
  auto* b = static_cast<VkBufferR*>(buffer);
  if (b && b->map && data) std::memcpy(static_cast<char*>(b->map) + offset, data, size);
}
void VkDeviceR::destroy_buffer(Buffer* buffer) {
  if (!buffer) return;
  auto* b = static_cast<VkBufferR*>(buffer);
  if (b->map && b->memory) vkUnmapMemory(device_, b->memory);
  destroy_buffer_raw(b->buffer, b->memory);
  delete b;
}

Texture* VkDeviceR::create_texture(const TextureDesc& desc, const void* rgba) {
  auto* t = new VkTextureR();
  t->width = desc.width;
  t->height = desc.height;
  t->depth = desc.format == TextureFormat::Depth;
  t->format = t->depth ? VK_FORMAT_D32_SFLOAT : color_format_;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = t->format;
  ii.extent = {static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (t->depth) ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  else ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  vkCreateImage(device_, &ii, nullptr, &t->image);
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, t->image, &req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device_, &ai, nullptr, &t->memory);
  vkBindImageMemory(device_, t->image, t->memory, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = t->image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = t->format;
  vi.subresourceRange.aspectMask = t->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.layerCount = 1;
  vkCreateImageView(device_, &vi, nullptr, &t->view);
  if (!t->depth) {
    std::vector<unsigned char> staging_pixels;
    const void* src = rgba;
    if (!src) {
      staging_pixels.assign(static_cast<std::size_t>(desc.width) * desc.height * 4u, 0);
      src = staging_pixels.data();
    }
    VkDeviceMemory sm = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceSize bytes = static_cast<VkDeviceSize>(desc.width) * desc.height * 4;
    VkBuffer sb = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sm, &map);
    if (map) std::memcpy(map, src, static_cast<std::size_t>(bytes));
    one_shot([&](VkCommandBuffer c) {
      transition(c, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false);
      VkBufferImageCopy cp{};
      cp.imageExtent = {static_cast<uint32_t>(desc.width), static_cast<uint32_t>(desc.height), 1};
      cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      cp.imageSubresource.layerCount = 1;
      vkCmdCopyBufferToImage(c, sb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
      transition(c, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    });
    if (map) vkUnmapMemory(device_, sm);
    destroy_buffer_raw(sb, sm);
  } else {
    one_shot([&](VkCommandBuffer c) {
      transition(c, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true);
    });
  }
  return t;
}
void VkDeviceR::update_texture(Texture* texture, const void* rgba, int w, int h) {
  auto* t = static_cast<VkTextureR*>(texture);
  if (!t || t->depth || !rgba) return;
  t->width = w;
  t->height = h;
  VkDeviceMemory sm = VK_NULL_HANDLE;
  void* map = nullptr;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
  VkBuffer sb = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sm, &map);
  if (map) std::memcpy(map, rgba, static_cast<std::size_t>(bytes));
  one_shot([&](VkCommandBuffer c) {
    transition(c, t->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false);
    VkBufferImageCopy cp{};
    cp.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    vkCmdCopyBufferToImage(c, sb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    transition(c, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
  });
  if (map) vkUnmapMemory(device_, sm);
  destroy_buffer_raw(sb, sm);
}
void VkDeviceR::destroy_texture(Texture* texture) {
  if (!texture || !device_) return;
  auto* t = static_cast<VkTextureR*>(texture);
  if (t->imgui && texture_unregistrar()) texture_unregistrar()(Api::Vulkan, t->imgui, texture_registrar_user());
  if (t->view) vkDestroyImageView(device_, t->view, nullptr);
  if (t->image) vkDestroyImage(device_, t->image, nullptr);
  if (t->memory) vkFreeMemory(device_, t->memory, nullptr);
  delete t;
}
std::uint64_t VkDeviceR::imgui_texture(Texture* texture) {
  auto* t = static_cast<VkTextureR*>(texture);
  if (!t) return 0;
  if (!t->imgui && texture_registrar()) t->imgui = texture_registrar()(Api::Vulkan, t->view, texture_registrar_user());
  return t->imgui;
}
Sampler* VkDeviceR::create_sampler(const SamplerDesc& desc) {
  auto* s = new VkSamplerR();
  s->desc = desc;
  s->sampler = sampler_of(desc);
  return s;
}
void VkDeviceR::destroy_sampler(Sampler* sampler) { delete static_cast<VkSamplerR*>(sampler); }

Pipeline* VkDeviceR::create_pipeline(const PipelineDesc& desc, std::string* error) {
  const ShaderCode* vs = find_shader(desc.vs);
  const ShaderCode* fs = find_shader(desc.fs);
  if (!vs || !fs) {
    if (error) *error = "missing baked shader";
    return nullptr;
  }
  auto mod = [&](const unsigned* code, unsigned words) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = words * 4u;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &mi, nullptr, &m);
    return m;
  };
  VkShaderModule vsm = mod(vs->spv, vs->spv_words);
  VkShaderModule fsm = mod(fs->spv, fs->spv_words);
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vsm;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fsm;
  VkVertexInputBindingDescription binds[2]{};
  int nb = 0;
  if (desc.strides[0]) {
    binds[nb].binding = 0;
    binds[nb].stride = desc.strides[0];
    binds[nb].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    ++nb;
  }
  if (desc.strides[1]) {
    binds[nb].binding = 1;
    binds[nb].stride = desc.strides[1];
    binds[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    ++nb;
  }
  VkVertexInputAttributeDescription attrs[8]{};
  for (int i = 0; i < desc.attr_count; ++i) {
    attrs[i].location = desc.attrs[i].location;
    attrs[i].binding = desc.attrs[i].buffer;
    attrs[i].offset = desc.attrs[i].offset;
    attrs[i].format = desc.attrs[i].components == 4   ? VK_FORMAT_R32G32B32A32_SFLOAT
                      : desc.attrs[i].components == 2 ? VK_FORMAT_R32G32_SFLOAT
                      : desc.attrs[i].components == 1 ? VK_FORMAT_R32_SFLOAT
                                                      : VK_FORMAT_R32G32B32_SFLOAT;
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = static_cast<uint32_t>(nb);
  vi.pVertexBindingDescriptions = binds;
  vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(desc.attr_count);
  vi.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = desc.topology == Topology::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = desc.cull == Cull::None ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1;
  rs.depthBiasEnable = desc.polygon_offset ? VK_TRUE : VK_FALSE;
  rs.depthBiasConstantFactor = 2.0f;
  rs.depthBiasSlopeFactor = 1.5f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = desc.depth_cmp == Compare::Always ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState ba{};
  ba.colorWriteMask = 0xF;
  if (desc.blend != Blend::Off) {
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = desc.blend == Blend::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &ba;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyn;
  const bool depth_only = desc.fs && std::strcmp(desc.fs, "depth.frag") == 0;
  if (depth_only) cb.attachmentCount = 0;
  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.stageCount = 2;
  pi.pStages = stages;
  pi.pVertexInputState = &vi;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pDepthStencilState = &ds;
  pi.pColorBlendState = &cb;
  pi.pDynamicState = &dy;
  pi.layout = desc.ray_query && rt_pipe_layout_ ? rt_pipe_layout_ : pipe_layout_;
  pi.renderPass = depth_only ? depth_pass_ : color_pass_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult pr = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
  vkDestroyShaderModule(device_, vsm, nullptr);
  vkDestroyShaderModule(device_, fsm, nullptr);
  if (pr != VK_SUCCESS) {
    if (error) *error = "vkCreateGraphicsPipelines failed";
    return nullptr;
  }
  auto* out = new VkPipelineR();
  out->pipeline = pipeline;
  out->layout = pi.layout;
  out->depth_only = depth_only;
  out->ray = desc.ray_query;
  out->desc = desc;
  return out;
}
void VkDeviceR::destroy_pipeline(Pipeline* pipeline) {
  if (!pipeline) return;
  auto* p = static_cast<VkPipelineR*>(pipeline);
  vkDestroyPipeline(device_, p->pipeline, nullptr);
  delete p;
}

RenderTarget* VkDeviceR::create_target(const TargetDesc& desc, std::string* error) {
  auto* t = new VkTargetR();
  t->width = desc.width;
  t->height = desc.height;
  t->color_on = desc.color;
  TextureDesc td;
  td.width = desc.width;
  td.height = desc.height;
  if (desc.color) t->color = static_cast<VkTextureR*>(create_texture(td, nullptr));
  if (desc.depth) {
    td.format = TextureFormat::Depth;
    t->depth = static_cast<VkTextureR*>(create_texture(td, nullptr));
  }
  VkImageView views[2]{};
  int n = 0;
  if (t->color) views[n++] = t->color->view;
  if (t->depth) views[n++] = t->depth->view;
  VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fi.renderPass = t->color_on ? color_pass_ : depth_pass_;
  fi.attachmentCount = static_cast<uint32_t>(n);
  fi.pAttachments = views;
  fi.width = static_cast<uint32_t>(desc.width);
  fi.height = static_cast<uint32_t>(desc.height);
  fi.layers = 1;
  if (vkCreateFramebuffer(device_, &fi, nullptr, &t->fb) != VK_SUCCESS) {
    if (error) *error = "Vulkan framebuffer failed";
    destroy_target(t);
    return nullptr;
  }
  return t;
}
void VkDeviceR::destroy_target(RenderTarget* target) {
  if (!target) return;
  auto* t = static_cast<VkTargetR*>(target);
  if (t->fb) vkDestroyFramebuffer(device_, t->fb, nullptr);
  destroy_texture(t->color);
  destroy_texture(t->depth);
  delete t;
}

void VkDeviceR::begin_frame() {
  if (depth_++ > 0) return;
  vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &fence_);
  vkResetDescriptorPool(device_, desc_pool_, 0);
  vkResetCommandPool(device_, pool_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command_, &bi);
  recording_ = true;
  cmd_.dev = this;
  cmd_.cmd = command_;
  cmd_.pass = false;
  if (swap_ && acquire_fence_) {
    vkAcquireNextImageKHR(device_, swap_, UINT64_MAX, VK_NULL_HANDLE, acquire_fence_, &image_index_);
    vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &acquire_fence_);
  }
  if (window_) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w > 0 && h > 0) { width_ = w; height_ = h; }
  }
}
void VkDeviceR::end_frame() {
  if (depth_ == 0) return;
  if (--depth_ > 0) return;
  if (cmd_.pass) cmd_.end_pass();
  vkEndCommandBuffer(command_);
  recording_ = false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &command_;
  vkQueueSubmit(queue_, 1, &si, fence_);
  vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  if (swap_) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap_;
    pi.pImageIndices = &image_index_;
    vkQueuePresentKHR(queue_, &pi);
  }
}
bool VkDeviceR::read_color(RenderTarget* target, std::vector<std::uint8_t>* rgba, int* w, int* h) {
  auto* t = static_cast<VkTargetR*>(target);
  if (!t || !t->color || !rgba) return false;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(t->width) * t->height * 4;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  void* map = nullptr;
  VkBuffer buf = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mem, &map);
  one_shot([&](VkCommandBuffer c) {
    transition(c, t->color->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false);
    VkBufferImageCopy cp{};
    cp.imageExtent = {static_cast<uint32_t>(t->width), static_cast<uint32_t>(t->height), 1};
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    vkCmdCopyImageToBuffer(c, t->color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    transition(c, t->color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
  });
  rgba->resize(static_cast<std::size_t>(bytes));
  // Image memory is top-down. The renderer contract is bottom row first.
  if (map) {
    const std::size_t row = static_cast<std::size_t>(t->width) * 4u;
    for (int y = 0; y < t->height; ++y) {
      std::memcpy(rgba->data() + static_cast<std::size_t>(t->height - 1 - y) * row,
                  static_cast<const char*>(map) + static_cast<std::size_t>(y) * row, row);
    }
  }
  vkUnmapMemory(device_, mem);
  destroy_buffer_raw(buf, mem);
  if (w) *w = t->width;
  if (h) *h = t->height;
  return true;
}

std::uint32_t VkDeviceR::rt_create_blas(const RtGeometry& geo, std::string* error) {
  if (!raytracing_ || !geo.vertices || !geo.indices) {
    if (error) *error = "ray tracing is not available on this Vulkan device";
    return 0;
  }
  auto addr_of = [&](Buffer* b) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = static_cast<VkBufferR*>(b)->buffer;
    return vkGetBufferDeviceAddress(device_, &info);
  };
  VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  geom.geometry.triangles.vertexData.deviceAddress = addr_of(geo.vertices);
  geom.geometry.triangles.vertexStride = geo.stride;
  geom.geometry.triangles.maxVertex = geo.vertex_count - 1;
  geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
  geom.geometry.triangles.indexData.deviceAddress = addr_of(geo.indices);
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geom;
  const std::uint32_t prims = geo.index_count / 3;
  VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &prims, &sizes);
  AsObj obj;
  obj.buffer = make_buffer(sizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &obj.memory, nullptr);
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = obj.buffer;
  ci.size = sizes.accelerationStructureSize;
  ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  vkCreateAccelerationStructureKHR(device_, &ci, nullptr, &obj.as);
  VkDeviceMemory scratch_mem = VK_NULL_HANDLE;
  VkBuffer scratch = make_buffer(sizes.buildScratchSize,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scratch_mem, nullptr);
  VkBufferDeviceAddressInfo sai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  sai.buffer = scratch;
  build.dstAccelerationStructure = obj.as;
  build.scratchData.deviceAddress = vkGetBufferDeviceAddress(device_, &sai);
  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = prims;
  const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
  one_shot([&](VkCommandBuffer c) { vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &pr); });
  destroy_buffer_raw(scratch, scratch_mem);
  VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  dai.accelerationStructure = obj.as;
  obj.address = vkGetAccelerationStructureDeviceAddressKHR(device_, &dai);
  ases_.push_back(obj);
  return static_cast<std::uint32_t>(ases_.size());
}

std::uint32_t VkDeviceR::rt_create_tlas(const RtInstance* instances, std::uint32_t count, std::string* error) {
  if (!raytracing_ || !count) {
    if (error) *error = "ray tracing is not available on this Vulkan device";
    return 0;
  }
  std::vector<VkAccelerationStructureInstanceKHR> inst(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::memcpy(&inst[i].transform, instances[i].transform, sizeof(inst[i].transform));
    const std::uint32_t id = instances[i].blas;
    inst[i].accelerationStructureReference = (id > 0 && id <= ases_.size()) ? ases_[id - 1].address : 0;
    inst[i].mask = 0xFF;
    inst[i].instanceCustomIndex = i;
  }
  VkDeviceMemory im = VK_NULL_HANDLE;
  void* map = nullptr;
  VkBuffer ib = make_buffer(sizeof(VkAccelerationStructureInstanceKHR) * count,
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &im, &map);
  if (map) std::memcpy(map, inst.data(), sizeof(VkAccelerationStructureInstanceKHR) * count);
  VkBufferDeviceAddressInfo iai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  iai.buffer = ib;
  VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geom.geometry.instances.arrayOfPointers = VK_FALSE;
  geom.geometry.instances.data.deviceAddress = vkGetBufferDeviceAddress(device_, &iai);
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geom;
  VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count, &sizes);
  AsObj obj;
  obj.buffer = make_buffer(sizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &obj.memory, nullptr);
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = obj.buffer;
  ci.size = sizes.accelerationStructureSize;
  ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  vkCreateAccelerationStructureKHR(device_, &ci, nullptr, &obj.as);
  VkDeviceMemory scratch_mem = VK_NULL_HANDLE;
  VkBuffer scratch = make_buffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scratch_mem, nullptr);
  VkBufferDeviceAddressInfo sai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  sai.buffer = scratch;
  build.dstAccelerationStructure = obj.as;
  build.scratchData.deviceAddress = vkGetBufferDeviceAddress(device_, &sai);
  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = count;
  const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
  one_shot([&](VkCommandBuffer c) { vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &pr); });
  destroy_buffer_raw(scratch, scratch_mem);
  if (map) vkUnmapMemory(device_, im);
  destroy_buffer_raw(ib, im);
  ases_.push_back(obj);
  return static_cast<std::uint32_t>(ases_.size());
}
void VkDeviceR::rt_reset() {
  for (AsObj& a : ases_) {
    if (a.as) vkDestroyAccelerationStructureKHR(device_, a.as, nullptr);
    destroy_buffer_raw(a.buffer, a.memory);
  }
  ases_.clear();
}
bool VkDeviceR::vk_info(VkInfo* out) const {
  if (!out) return false;
  out->instance = instance_;
  out->physical = physical_;
  out->device = device_;
  out->queue = queue_;
  out->family = family_;
  out->render_pass = swap_pass_ ? swap_pass_ : color_pass_;
  out->command = command_;
  out->framebuffer = current_swap_fb();
  out->images = image_count_ ? image_count_ : 2;
  out->api_version = VK_API_VERSION_1_2;
  out->width = width_;
  out->height = height_;
  return true;
}

std::unique_ptr<Device> create_vk_device(const DeviceDesc& desc, std::string* error) {
  auto dev = std::make_unique<VkDeviceR>();
  if (!dev->open(desc, error)) return nullptr;
  return dev;
}
