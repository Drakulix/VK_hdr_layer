#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include "vkroots.h"
#include "color-management-v1-client-protocol.h"
#include "color-representation-v1-client-protocol.h"
#include "color-bypass-xwayland-client-protocol.h"

#include <X11/Xlib-xcb.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <optional>

using namespace std::literals;

namespace HdrLayer
{
  static bool contains_str(const std::vector<const char *> vec, std::string_view lookupValue)
  {
    return std::find_if(vec.begin(), vec.end(),
                        [=](const char *value)
                        { return value == lookupValue; }) != vec.end();
  }
  static bool contains_u32(const std::vector<uint32_t> vec, uint32_t lookupValue)
  {
    return std::find_if(vec.begin(), vec.end(),
                        [=](uint32_t value)
                        { return value == lookupValue; }) != vec.end();
  }

  struct WaylandConnectionData
  {
    wl_display *display;
    wl_event_queue *queue;
    wp_color_manager_v1 *colorManagement;
    wp_color_representation_manager_v1 *colorRepresentation;
    zcolor_bypass_xwayland *colorXwayland;

    std::vector<uint32_t> features;
    std::vector<uint32_t> tf_cicp;
    std::vector<uint32_t> primaries_cicp;
  };

  struct HdrInstanceData
  {
    WaylandConnectionData globalConn;
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrInstance, VkInstance);

  struct HdrSurfaceData
  {
    VkInstance instance;
    WaylandConnectionData surfaceConn;
    wp_color_management_surface_v1 *colorSurface;
    wp_color_representation_v1 *colorRepresentation;

    xcb_connection_t *connection;
    xcb_window_t window;
    wl_surface *surface;
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrSurface, VkSurfaceKHR);

  struct HdrSwapchainData
  {
    VkSurfaceKHR surface;
    wp_image_description_v1 *colorDescription;
    bool desc_dirty;
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrSwapchain, VkSwapchainKHR);

  class VkInstanceOverrides
  {
  public:
    static VkResult CreateInstance(
        PFN_vkCreateInstance pfnCreateInstanceProc,
        const VkInstanceCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkInstance *pInstance)
    {
      auto enabledExts = std::vector<const char *>(
          pCreateInfo->ppEnabledExtensionNames,
          pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);

      if (!contains_str(enabledExts, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME))
        enabledExts.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

      if (contains_str(enabledExts, VK_KHR_XLIB_SURFACE_EXTENSION_NAME))
        enabledExts.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);

      VkInstanceCreateInfo createInfo = *pCreateInfo;
      createInfo.enabledExtensionCount = uint32_t(enabledExts.size());
      createInfo.ppEnabledExtensionNames = enabledExts.data();

      VkResult result = pfnCreateInstanceProc(&createInfo, pAllocator, pInstance);
      if (result != VK_SUCCESS)
        return result;

      wl_display *display = wl_display_connect(nullptr);
      if (!display)
      {
        fprintf(stderr, "[HDR Layer] Failed to connect to wayland socket.\n");
        return result;
      }

      auto connectionState = WaylandConnectionData{
          .display = display,
          .features = {},
          .tf_cicp = {},
          .primaries_cicp = {},
      };
      InitializeWaylandState(&connectionState);

      auto state = HdrInstance::create(*pInstance, HdrInstanceData{
                                                       .globalConn = connectionState,
                                                   });

      return result;
    }

    static void DestroyInstance(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        const VkAllocationCallbacks *pAllocator)
    {
      if (auto state = HdrInstance::get(instance))
      {
        wl_display_disconnect(state->globalConn.display);
      }
      HdrInstance::remove(instance);
      pDispatch->DestroyInstance(instance, pAllocator);
    }

    static VkResult CreateWaylandSurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
    {
      auto hdrInstance = HdrInstance::get(instance);
      if (!hdrInstance)
        return pDispatch->CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      // new wayland-connection
      auto queue = wl_display_create_queue(pCreateInfo->display);
      auto connState = WaylandConnectionData{
          .display = pCreateInfo->display,
          .queue = queue,
          .features = {},
          .tf_cicp = {},
          .primaries_cicp = {},
      };
      InitializeWaylandState(&connState);

      auto surface = pCreateInfo->surface;

      wp_color_management_surface_v1 *colorSurface = wp_color_manager_v1_get_color_management_surface(connState.colorManagement, surface);
      wp_color_management_surface_v1_add_listener(colorSurface, &color_surface_interface_listener, nullptr);
      wp_color_representation_v1 *colorRepresentation = wp_color_representation_manager_v1_create(connState.colorRepresentation, surface);
      wl_display_flush(connState.display);

      VkResult result = pDispatch->CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
      if (result != VK_SUCCESS)
      {
        fprintf(stderr, "[HDR Layer] Failed to create Vulkan wayland surface - vr: %s\n", vkroots::helpers::enumString(result));
        return result;
      }

      auto hdrSurface = HdrSurface::create(*pSurface, HdrSurfaceData{
                                                          .instance = instance,
                                                          .surfaceConn = connState,
                                                          .colorSurface = colorSurface,
                                                          .colorRepresentation = colorRepresentation,
                                                          .connection = nullptr,
                                                          .window = 0,
                                                          .surface = surface,
                                                      });

      DumpHdrSurfaceState(hdrSurface);

      return VK_SUCCESS;
    }

    static VkBool32 GetPhysicalDeviceXcbPresentationSupportKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        uint32_t queueFamilyIndex,
        xcb_connection_t *connection,
        xcb_visualid_t visual_id)
    {
      auto hdrInstance = HdrInstance::get(pDispatch->Instance);
      if (!hdrInstance || !hdrInstance->globalConn.colorXwayland)
        return pDispatch->GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice, queueFamilyIndex, connection, visual_id);

      return GetPhysicalDevicePresentationSupport(pDispatch, hdrInstance, physicalDevice, queueFamilyIndex);
    }

    static VkBool32 GetPhysicalDeviceXlibPresentationSupportKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        uint32_t queueFamilyIndex,
        Display *dpy,
        VisualID visualID)
    {
      auto hdrInstance = HdrInstance::get(pDispatch->Instance);
      if (!hdrInstance || !hdrInstance->globalConn.colorXwayland)
        return pDispatch->GetPhysicalDeviceXlibPresentationSupportKHR(physicalDevice, queueFamilyIndex, dpy, visualID);

      return GetPhysicalDevicePresentationSupport(pDispatch, hdrInstance, physicalDevice, queueFamilyIndex);
    }

    static VkResult CreateXcbSurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
    {
      auto hdrInstance = HdrInstance::get(instance);
      if (!hdrInstance || !hdrInstance->globalConn.colorXwayland)
        return pDispatch->CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      auto connection = pCreateInfo->connection;
      auto window = pCreateInfo->window;

      fprintf(stderr, "[HDR Layer] Creating color_management surface: xid: 0x%x\n", window);

      wp_color_management_surface_v1 *colorSurface = zcolor_bypass_xwayland_get_color_management_surface(hdrInstance->globalConn.colorXwayland, pCreateInfo->window);
      wp_color_management_surface_v1_add_listener(colorSurface, &color_surface_interface_listener, nullptr);
      wp_color_representation_v1 *colorRepresentation = zcolor_bypass_xwayland_get_color_representation(hdrInstance->globalConn.colorXwayland, pCreateInfo->window);
      wl_display_flush(hdrInstance->globalConn.display);

      VkResult result = pDispatch->CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
      if (result != VK_SUCCESS)
      {
        fprintf(stderr, "[HDR Layer] Failed to create Vulkan xcb surface - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), window);
        return result;
      }

      auto hdrSurface = HdrSurface::create(*pSurface, HdrSurfaceData{
                                                          .instance = instance,
                                                          .colorSurface = colorSurface,
                                                          .colorRepresentation = colorRepresentation,
                                                          .connection = connection,
                                                          .window = window,
                                                          .surface = nullptr,
                                                      });

      DumpHdrSurfaceState(hdrSurface);

      return VK_SUCCESS;
    }

    static VkResult CreateXlibSurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
    {
      auto hdrInstance = HdrInstance::get(instance);
      if (!hdrInstance || !hdrInstance->globalConn.colorXwayland)
        return pDispatch->CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      auto connection = XGetXCBConnection(pCreateInfo->dpy);
      auto window = xcb_window_t(pCreateInfo->window);

      fprintf(stderr, "[HDR Layer] Creating color_management surface: xid: 0x%x\n", window);

      wp_color_management_surface_v1 *colorSurface = zcolor_bypass_xwayland_get_color_management_surface(hdrInstance->globalConn.colorXwayland, pCreateInfo->window);
      wp_color_management_surface_v1_add_listener(colorSurface, &color_surface_interface_listener, nullptr);
      wp_color_representation_v1 *colorRepresentation = zcolor_bypass_xwayland_get_color_representation(hdrInstance->globalConn.colorXwayland, pCreateInfo->window);
      wl_display_flush(hdrInstance->globalConn.display);

      VkResult result = pDispatch->CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
      if (result != VK_SUCCESS)
      {
        fprintf(stderr, "[HDR Layer] Failed to create Vulkan xcb surface - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), window);
        return result;
      }

      auto hdrSurface = HdrSurface::create(*pSurface, HdrSurfaceData{
                                                          .instance = instance,
                                                          .colorSurface = colorSurface,
                                                          .colorRepresentation = colorRepresentation,
                                                          .connection = connection,
                                                          .window = window,
                                                          .surface = nullptr,
                                                      });

      DumpHdrSurfaceState(hdrSurface);

      return VK_SUCCESS;
    }

    static constexpr std::array<VkSurfaceFormat2KHR, 2> s_ExtraHDRSurfaceFormat2sHDR10 = {{
        {.surfaceFormat = {
             VK_FORMAT_A2B10G10R10_UNORM_PACK32,
             VK_COLOR_SPACE_HDR10_ST2084_EXT,
         }},
        {.surfaceFormat = {
             VK_FORMAT_A2R10G10B10_UNORM_PACK32,
             VK_COLOR_SPACE_HDR10_ST2084_EXT,
         }},
    }};
    static constexpr std::array<VkSurfaceFormat2KHR, 1> s_ExtraHDRSurfaceFormat2sLINEAR = {{
        {.surfaceFormat = {
             VK_FORMAT_R16G16B16A16_SFLOAT,
             VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
         }},
    }};

    static VkResult GetPhysicalDeviceSurfaceFormatsKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t *pSurfaceFormatCount,
        VkSurfaceFormatKHR *pSurfaceFormats)
    {
      auto hdrInstance = HdrInstance::get(pDispatch->Instance);
      auto hdrSurface = HdrSurface::get(surface);
      if (!hdrInstance || !hdrSurface)
        return pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);

      auto waylandConn = (hdrSurface->surfaceConn.display) ? &hdrSurface->surfaceConn : &hdrInstance->globalConn;

      std::vector<VkSurfaceFormatKHR> extraFormats = {};
      if (contains_u32(waylandConn->tf_cicp, 16) && contains_u32(waylandConn->primaries_cicp, 9))
      {
        for (uint64_t i = 0; i < s_ExtraHDRSurfaceFormat2sHDR10.size(); i++)
        {
          extraFormats.push_back(s_ExtraHDRSurfaceFormat2sHDR10[i].surfaceFormat);
        }
      }
      if (contains_u32(waylandConn->tf_cicp, 8) && contains_u32(waylandConn->primaries_cicp, 1) && contains_u32(waylandConn->features, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME))
      {
        for (uint64_t i = 0; i < s_ExtraHDRSurfaceFormat2sLINEAR.size(); i++)
        {
          extraFormats.push_back(s_ExtraHDRSurfaceFormat2sLINEAR[i].surfaceFormat);
        }
      }

      return vkroots::helpers::append(
          pDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
          extraFormats,
          pSurfaceFormatCount,
          pSurfaceFormats,
          physicalDevice,
          surface);
    }

    static VkResult GetPhysicalDeviceSurfaceFormats2KHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
        uint32_t *pSurfaceFormatCount,
        VkSurfaceFormat2KHR *pSurfaceFormats)
    {
      auto hdrInstance = HdrInstance::get(pDispatch->Instance);
      auto hdrSurface = HdrSurface::get(pSurfaceInfo->surface);
      if (!hdrInstance || !hdrSurface)
        return pDispatch->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);

      auto waylandConn = (hdrSurface->surfaceConn.display) ? &hdrSurface->surfaceConn : &hdrInstance->globalConn;

      std::vector<VkSurfaceFormat2KHR> extraFormats = {};
      if (contains_u32(waylandConn->tf_cicp, 16) && contains_u32(waylandConn->primaries_cicp, 9))
      {
        for (uint64_t i = 0; i < s_ExtraHDRSurfaceFormat2sHDR10.size(); i++)
        {
          extraFormats.push_back(s_ExtraHDRSurfaceFormat2sHDR10[i]);
        }
      }
      if (contains_u32(waylandConn->tf_cicp, 8) && contains_u32(waylandConn->primaries_cicp, 1) && contains_u32(waylandConn->features, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME))
      {
        for (uint64_t i = 0; i < s_ExtraHDRSurfaceFormat2sLINEAR.size(); i++)
        {
          extraFormats.push_back(s_ExtraHDRSurfaceFormat2sLINEAR[i]);
        }
      }

      return vkroots::helpers::append(
          pDispatch->GetPhysicalDeviceSurfaceFormats2KHR,
          extraFormats,
          pSurfaceFormatCount,
          pSurfaceFormats,
          physicalDevice,
          pSurfaceInfo);
    }

    static void DestroySurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        VkSurfaceKHR surface,
        const VkAllocationCallbacks *pAllocator)
    {
      if (auto state = HdrSurface::get(surface))
      {
        // TODO destroy other objects?
        wl_surface_destroy(state->surface);
      }
      HdrSurface::remove(surface);
      pDispatch->DestroySurfaceKHR(instance, surface, pAllocator);
    }

    static VkResult EnumerateDeviceExtensionProperties(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        const char *pLayerName,
        uint32_t *pPropertyCount,
        VkExtensionProperties *pProperties)
    {
      // TODO: ColorSpace extensions? https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkColorSpaceKHR.html
      static constexpr std::array<VkExtensionProperties, 1> s_LayerExposedExts = {{
          {VK_EXT_HDR_METADATA_EXTENSION_NAME,
           VK_EXT_HDR_METADATA_SPEC_VERSION},
      }};

      if (pLayerName)
      {
        if (pLayerName == "VK_LAYER_hdr_wsi"sv)
        {
          return vkroots::helpers::array(s_LayerExposedExts, pPropertyCount, pProperties);
        }
        else
        {
          return pDispatch->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
        }
      }

      return vkroots::helpers::append(
          pDispatch->EnumerateDeviceExtensionProperties,
          s_LayerExposedExts,
          pPropertyCount,
          pProperties,
          physicalDevice,
          pLayerName);
    }

  private:
    static VkResult InitializeWaylandState(WaylandConnectionData *conn)
    {
      wl_registry *registry = wl_display_get_registry(conn->display);
      if (conn->queue)
      {
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), conn->queue);
      }
      wl_registry_add_listener(registry, &s_registryListener, reinterpret_cast<void *>(conn));

      if (conn->queue)
      {
        wl_display_dispatch_queue(conn->display, conn->queue);
        wl_display_roundtrip_queue(conn->display, conn->queue); // get globals
        wl_display_roundtrip_queue(conn->display, conn->queue); // get features/supported_cicps/etc
      }
      else
      {
        wl_display_dispatch(conn->display);
        wl_display_roundtrip(conn->display); // get globals
        wl_display_roundtrip(conn->display); // get features/supported_cicps/etc
      }
      wl_registry_destroy(registry);

      if (!conn->colorXwayland)
      {
        fprintf(stderr, "[HDR Layer] wayland compositor lacking bypass protocol, only wayland surfaces will be supported.\n");
      }
      if (!conn->colorManagement)
      {
        fprintf(stderr, "[HDR Layer] wayland compositor lacking color management protocol..\n");
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      if (!contains_u32(conn->features, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC))
      {
        fprintf(stderr, "[HDR Layer] color management implementation doesn't support parametric image descriptions..\n");
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      if (!contains_u32(conn->features, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES))
      {
        fprintf(stderr, "[HDR Layer] color management implementation doesn't support SET_PRIMARIES..\n");
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      if (!contains_u32(conn->features, WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER))
      {
        fprintf(stderr, "[HDR Layer] color management implementation doesn't support SET_TF_POWER..\n");
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      if (!conn->colorRepresentation)
      {
        fprintf(stderr, "[HDR Layer] wayland compositor lacking color representation protocol..\n");
        return VK_ERROR_INITIALIZATION_FAILED;
      }

      return VK_SUCCESS;
    }

    static void DumpHdrSurfaceState(HdrSurface &surface)
    {
      fprintf(stderr, "[HDR Layer] Surface state:\n");
      fprintf(stderr, "  wayland surface res id:        %u\n", wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(surface->surface)));
      fprintf(stderr, "  window xid:                    0x%x\n", surface->window);
    }

    static VkBool32 GetPhysicalDevicePresentationSupport(
        const vkroots::VkInstanceDispatch *pDispatch,
        HdrInstance &hdrInstance,
        VkPhysicalDevice physicalDevice,
        uint32_t queueFamilyIndex)
    {
      return pDispatch->GetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, hdrInstance->globalConn.display);
    }

    static constexpr struct wp_color_manager_v1_listener color_interface_listener
    {
      .supported_intent = [](void *data,
                             struct wp_color_manager_v1 *wp_color_manager_v1,
                             uint32_t render_intent) {},
      .supported_feature = [](void *data,
                              struct wp_color_manager_v1 *wp_color_manager_v1,
                              uint32_t feature)
      {
        auto instance = reinterpret_cast<WaylandConnectionData *>(data);
        instance->features.push_back(feature);
      },
      .supported_tf_cicp = [](void *data, struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t tf_code)
      {
        auto instance = reinterpret_cast<WaylandConnectionData *>(data);
        instance->tf_cicp.push_back(tf_code);
      },
      .supported_primaries_cicp = [](void *data, struct wp_color_manager_v1 *wp_color_manager_v1, uint32_t primaries_code)
      {
        auto instance = reinterpret_cast<WaylandConnectionData *>(data);
        instance->primaries_cicp.push_back(primaries_code);
      }
    };

    static constexpr struct wp_color_representation_manager_v1_listener representation_interface_listener
    {
      .coefficients = [](void *data,
                         struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1,
                         uint32_t code_point) {},
      .chroma_location = [](void *data,
                            struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1,
                            uint32_t code_point) {}
    };

    static constexpr struct wp_color_management_surface_v1_listener color_surface_interface_listener
    {
      .preferred_changed = [](void *data,
                              struct wp_color_management_surface_v1 *wp_color_management_surface_v1) {}
    };

    static constexpr struct wp_image_description_v1_listener image_description_interface_listener
    {
      .failed = [](
                    void *data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t cause,
                    const char *msg)
      {
        fprintf(stderr, "[HDR Layer] Image description failed: Cause %u, message: %s.\n", cause, msg);
      },
      .ready = [](void *data, struct wp_image_description_v1 *wp_image_description_v1, uint32_t identity) {}
      // we don't call get_information, so the rest should never be called
    };

    static constexpr wl_registry_listener s_registryListener = {
        .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
        {
        auto instance = reinterpret_cast<WaylandConnectionData *>(data);

        if (interface == "zcolor_bypass_xwayland"sv) {
          instance->colorXwayland = reinterpret_cast<zcolor_bypass_xwayland *>(
            wl_registry_bind(registry, name, &zcolor_bypass_xwayland_interface, version));
        } else if (interface == "wp_color_manager_v1"sv) {
          instance->colorManagement = reinterpret_cast<wp_color_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_color_manager_v1_interface, version));
          wp_color_manager_v1_add_listener(instance->colorManagement, &color_interface_listener, data);
        } else if (interface == "wp_color_representation_v1"sv) {
          instance->colorRepresentation = reinterpret_cast<wp_color_representation_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_color_representation_v1_interface, version));
          wp_color_representation_manager_v1_add_listener(instance->colorRepresentation, &representation_interface_listener, nullptr);
        } },
        .global_remove = [](void *data, wl_registry *registry, uint32_t name) {},
    };
  };

  class VkDeviceOverrides
  {
  public:
    static void DestroySwapchainKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks *pAllocator)
    {
      HdrSwapchain::remove(swapchain);
      pDispatch->DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    static VkResult CreateSwapchainKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        const VkSwapchainCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSwapchainKHR *pSwapchain)
    {
      auto hdrSurface = HdrSurface::get(pCreateInfo->surface);
      if (!hdrSurface)
        return pDispatch->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

      VkSwapchainCreateInfoKHR swapchainInfo = *pCreateInfo;

      if (hdrSurface)
      {
        // If this is a custom surface
        // Force the colorspace to sRGB before sending to the driver.
        swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

        fprintf(stderr, "[HDR Layer] Creating swapchain for id: %u - format: %s - colorspace: %s\n",
                wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(hdrSurface->surface)),
                vkroots::helpers::enumString(pCreateInfo->imageFormat),
                vkroots::helpers::enumString(pCreateInfo->imageColorSpace));
      }

      // Check for VkFormat support and return VK_ERROR_INITIALIZATION_FAILED
      // if that VkFormat is unsupported for the underlying surface.
      {
        std::vector<VkSurfaceFormatKHR> supportedSurfaceFormats;
        vkroots::helpers::enumerate(
            pDispatch->pPhysicalDeviceDispatch->pInstanceDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
            supportedSurfaceFormats,
            pDispatch->PhysicalDevice,
            swapchainInfo.surface);

        bool supportedSwapchainFormat = std::find_if(
                                            supportedSurfaceFormats.begin(),
                                            supportedSurfaceFormats.end(),
                                            [=](VkSurfaceFormatKHR value)
                                            { return value.format == swapchainInfo.imageFormat; }) != supportedSurfaceFormats.end();

        if (!supportedSwapchainFormat)
        {
          fprintf(stderr, "[HDR Layer] Refusing to make swapchain (unsupported VkFormat) for id: %u - format: %s - colorspace: %s\n",
                  wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(hdrSurface->surface)),
                  vkroots::helpers::enumString(pCreateInfo->imageFormat),
                  vkroots::helpers::enumString(pCreateInfo->imageColorSpace));

          return VK_ERROR_INITIALIZATION_FAILED;
        }
      }

      VkResult result = pDispatch->CreateSwapchainKHR(device, &swapchainInfo, pAllocator, pSwapchain);
      if (hdrSurface)
      {
        if (result == VK_SUCCESS)
        {
          auto hdrInstance = HdrInstance::get(hdrSurface->instance);
          if (hdrInstance)
          {
            auto waylandConn = (hdrSurface->surfaceConn.display) ? &hdrSurface->surfaceConn : &hdrInstance->globalConn;
            if (pCreateInfo->compositeAlpha == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
            {
              wp_color_representation_v1_set_alpha_mode(hdrSurface->colorRepresentation, WP_COLOR_REPRESENTATION_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL);
            }
            else if (pCreateInfo->compositeAlpha == VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
            {
              wp_color_representation_v1_set_alpha_mode(hdrSurface->colorRepresentation, WP_COLOR_REPRESENTATION_V1_ALPHA_MODE_STRAIGHT);
            }

            auto primaries = 0;
            switch (swapchainInfo.imageColorSpace) {
              case VK_COLOR_SPACE_HDR10_ST2084_EXT:
                primaries = 9;
                break;
              case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
                primaries = 1;
                break;
              case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
                break;
              default:
                fprintf(stderr, "[HDR Layer] Unknown color space, assuming untagged");
            };
            
            wp_image_description_v1 *desc = nullptr;
            if (primaries != 0) {
              wp_image_description_creator_params_v1 *params = wp_color_manager_v1_new_parametric_creator(waylandConn->colorManagement);
              wp_image_description_creator_params_v1_set_primaries_cicp(params, primaries);
              wp_image_description_creator_params_v1_set_tf_cicp(params, 16);
              desc = wp_image_description_creator_params_v1_create(params);
              // wp_image_description_v1_add_listener(desc, &image_description_interface_listener, nullptr);
            }

            wl_display_roundtrip(waylandConn->display); // lets hope the description is ready now!
            
            HdrSwapchain::create(*pSwapchain, HdrSwapchainData{
                                                  .surface = pCreateInfo->surface,
                                                  .colorDescription = desc,
                                              });
          }
        }
        else
        {
          fprintf(stderr, "[HDR Layer] Failed to create swapchain - vr: %s id: %u\n", vkroots::helpers::enumString(result), wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(hdrSurface->surface)));
        }
      }
      return result;
    }

    static void SetHdrMetadataEXT(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        uint32_t swapchainCount,
        const VkSwapchainKHR *pSwapchains,
        const VkHdrMetadataEXT *pMetadata)
    {
      for (uint32_t i = 0; i < swapchainCount; i++)
      {
        auto hdrSwapchain = HdrSwapchain::get(pSwapchains[i]);
        if (!hdrSwapchain)
        {
          fprintf(stderr, "[HDR Layer] SetHdrMetadataEXT: Swapchain %u does not support HDR.\n", i);
          continue;
        }

        auto hdrSurface = HdrSurface::get(hdrSwapchain->surface);
        if (!hdrSurface)
        {
          fprintf(stderr, "[HDR Layer] SetHdrMetadataEXT: Surface for swapchain %u was already destroyed. (App use after free).\n", i);
          abort();
          continue;
        }

        auto hdrInstance = HdrInstance::get(hdrSurface->instance);
        if (!hdrInstance)
        {
          fprintf(stderr, "[HDR Layer] SetHdrMetadataEXT: Instance for swapchain %u was already destroyed. (App use after free).\n", i);
          abort();
          continue;
        }

        auto waylandConn = (hdrSurface->surfaceConn.display) ? &hdrSurface->surfaceConn : &hdrInstance->globalConn;

        const VkHdrMetadataEXT &metadata = pMetadata[i];
        wp_image_description_creator_params_v1 *params = wp_color_manager_v1_new_parametric_creator(waylandConn->colorManagement);
        wp_image_description_creator_params_v1_set_primaries(
            params,
            (uint32_t)round(metadata.displayPrimaryRed.x * 10000.0),
            (uint32_t)round(metadata.displayPrimaryRed.y * 10000.0),
            (uint32_t)round(metadata.displayPrimaryGreen.x * 10000.0),
            (uint32_t)round(metadata.displayPrimaryGreen.y * 10000.0),
            (uint32_t)round(metadata.displayPrimaryBlue.x * 10000.0),
            (uint32_t)round(metadata.displayPrimaryBlue.y * 10000.0),
            (uint32_t)round(metadata.whitePoint.x * 10000.0),
            (uint32_t)round(metadata.whitePoint.y * 10000.0));
        wp_image_description_creator_params_v1_set_mastering_luminance(
            params,
            (uint32_t)round(metadata.minLuminance * 10000.0),
            (uint32_t)round(metadata.maxLuminance));
        wp_image_description_creator_params_v1_set_tf_cicp(params, 16);
        wp_image_description_creator_params_v1_set_maxCLL(params, (uint32_t)round(metadata.maxContentLightLevel));
        wp_image_description_creator_params_v1_set_maxFALL(params, (uint32_t)round(metadata.maxFrameAverageLightLevel));
        wp_image_description_v1 *desc = wp_image_description_creator_params_v1_create(params);
        // wp_image_description_v1_add_listener(desc, &image_description_interface_listener, nullptr);

        wl_display_roundtrip(waylandConn->display); // lets hope the description is ready now!

        fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: mastering luminance min %f nits, max %f nits\n", metadata.minLuminance, metadata.maxLuminance);
        fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: maxContentLightLevel %f nits\n", metadata.maxContentLightLevel);
        fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: maxFrameAverageLightLevel %f nits\n", metadata.maxFrameAverageLightLevel);

        hdrSwapchain->colorDescription = desc;
        hdrSwapchain->desc_dirty = true;
      }
    }

    static VkResult QueuePresentKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkQueue queue,
        const VkPresentInfoKHR *pPresentInfo)
    {
      for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++)
      {
        if (auto hdrSwapchain = HdrSwapchain::get(pPresentInfo->pSwapchains[i]))
        {
          if (hdrSwapchain->desc_dirty)
          {
            auto hdrSurface = HdrSurface::get(hdrSwapchain->surface);
            if (hdrSwapchain->colorDescription)
            {
              wp_color_management_surface_v1_set_image_description(hdrSurface->colorSurface, hdrSwapchain->colorDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
            }
            else
            {
              wp_color_management_surface_v1_set_default_image_description(hdrSurface->colorSurface);
            }
            hdrSwapchain->desc_dirty = false;
          }
        }
      }

      return pDispatch->QueuePresentKHR(queue, pPresentInfo);
    }
  };

}

VKROOTS_DEFINE_LAYER_INTERFACES(HdrLayer::VkInstanceOverrides,
                                vkroots::NoOverrides,
                                HdrLayer::VkDeviceOverrides);

VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(HdrLayer::HdrInstance);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(HdrLayer::HdrSurface);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(HdrLayer::HdrSwapchain);
