## Vulkan Wayland HDR WSI Layer

Vulkan layer utilizing work-in-progress versions of the color management/representation protocols.
- [wp_color_management](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/14)
- [wp_color_representation](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/183)

Implements the following vulkan extensions by utilizing the listed wayland protocols, if made available by the compositor.
- [VK_EXT_swapchain_colorspace](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_swapchain_colorspace.html)
- [VK_EXT_hdr_metadata](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_hdr_metadata.html)

No compositor currently has a merged implementations of these protocols and no compositor should given these are snapshots of unfinished extensions.
This is for **testing purposes only**!

### Testing with gamescope

There aren't many vulkan clients to choose from right now, that run on wayland and can make use of the previously mentioned extensions. One of these clients is [`gamescope`](https://github.com/ValveSoftware/gamescope), which can run nested as a wayland client. As such it can forward HDR metadata of HDR windows games running inside of it via DXVK.

Given gamescope utilizes it's own vulkan layer and creative.. hacks to support this, setting up can be a bit convoluted.
You want to enable this layer for gamescope, but not for it's clients.

Here is an example command line (assuming this layer has been installed to the system as an implicit layer):
`env ENABLE_HDR_WSI=1 gamescope --hdr-enabled -- env DISABLE_HDR_WSI=1 steam -bigpicture`

Debugging what layers are being loaded can be done by setting `VK_LOADER_DEBUG=error,warn,info`.

Getting games to enable HDR might need `Proton Experimental` to be used in Steam as well as the following environment variables to be sure: `ENABLE_GAMESCOPE_WSI=1 DXVK_HDR=1`