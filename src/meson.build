vkroots_dep = dependency('vkroots')
wayland_client = dependency('wayland-client')

hdr_wsi_layer = shared_library('VkLayer_hdr_wsi', 'VkLayer_hdr_wsi.cpp', protocols_client_src,
  dependencies     : [ vkroots_dep, wayland_client ],
  install          : true )

out_lib_dir = join_paths(prefix, lib_dir)

configure_file(
    input         : 'VkLayer_hdr_wsi.json.in',
    output        : 'VkLayer_hdr_wsi.' + build_machine.cpu_family() + '.json',
    configuration : {'family' : build_machine.cpu_family(), 'lib_dir' : out_lib_dir },
    install       : true,
    install_dir   : join_paths(data_dir, 'vulkan', 'implicit_layer.d'),
)