/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "exec/icount.h"
#include "ui/xemu-notifications.h"
#include "ui/xemu-present.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "renderer.h"
#include "xemu-version.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include "metalfx_upscale.h"
#endif

#define VkExtensionPropertiesArray GArray
#define StringArray GArray

// TODO: MoltenVK Fix: change this when there's a better solution for MoltenVK.
/*
 * Older Vulkan headers may miss this extension name macro even when runtime
 * drivers expose it (e.g. MoltenVK portability subset).
 */
#ifndef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#define VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME "VK_KHR_portability_subset"
#endif

static char const *const validation_layers[] = {
    "VK_LAYER_KHRONOS_validation",
};

static char const *const required_device_extensions[] = {
#if HAVE_EXTERNAL_MEMORY
#ifdef WIN32
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    /*
     * No VK_KHR_external_semaphore_win32 (upstream requires it): the GL
     * interop present syncs with fences, and no semaphore handle crosses.
     */
#else
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#endif
#endif

    /*
     * Not VK_KHR_portability_subset: a portability driver (MoltenVK)
     * advertises it and must have it enabled, which
     * add_optional_device_extension_names() does; a conformant driver
     * (Mesa KosmicKrisp) doesn't advertise it and was rejected while it
     * sat in this list.
     */
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
{
    fprintf(stderr, "[vk] %s\n", pCallbackData->pMessage);

    if ((messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) &&
        (messageSeverity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) {
        assert(!g_config.display.vulkan.assert_on_validation_msg);
    }
    return VK_FALSE;
}

static bool check_validation_layer_support(void)
{
    uint32_t num_available_layers;
    vkEnumerateInstanceLayerProperties(&num_available_layers, NULL);

    g_autofree VkLayerProperties *available_layers =
        g_malloc_n(num_available_layers, sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&num_available_layers, available_layers);

    for (int i = 0; i < ARRAY_SIZE(validation_layers); i++) {
        bool found = false;
        for (int j = 0; j < num_available_layers; j++) {
            if (!strcmp(validation_layers[i], available_layers[j].layerName)) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "desired validation layer not found: %s\n",
                    validation_layers[i]);
            return false;
        }
    }

    return true;
}

static VkExtensionPropertiesArray *
get_available_instance_extensions(PGRAPHState *pg)
{
    uint32_t num_extensions = 0;

    if (vkEnumerateInstanceExtensionProperties(NULL, &num_extensions, NULL) !=
        VK_SUCCESS) {
        return NULL;
    }

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    if (vkEnumerateInstanceExtensionProperties(
            NULL, &num_extensions, (VkExtensionProperties *)extensions->data) !=
        VK_SUCCESS) {
        g_array_free(extensions, TRUE);
        return NULL;
    }

    return extensions;
}

static bool
is_extension_available(VkExtensionPropertiesArray *available_extensions,
                       const char *extension_name)
{
    for (int i = 0; i < available_extensions->len; i++) {
        VkExtensionProperties *e =
            &g_array_index(available_extensions, VkExtensionProperties, i);
        if (!strcmp(e->extensionName, extension_name)) {
            return true;
        }
    }

    return false;
}

static bool
add_extension_if_available(VkExtensionPropertiesArray *available_extensions,
                           StringArray *enabled_extension_names,
                           const char *desired_extension_name)
{
    if (is_extension_available(available_extensions, desired_extension_name)) {
        g_array_append_val(enabled_extension_names, desired_extension_name);
        return true;
    }

    fprintf(stderr, "Warning: extension not available: %s\n",
            desired_extension_name);
    return false;
}

static void
add_optional_instance_extension_names(PGRAPHState *pg,
                                      VkExtensionPropertiesArray *available_extensions,
                                      StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    r->debug_utils_extension_enabled =
        g_config.display.vulkan.validation_layers &&
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

#ifdef __APPLE__
    /* Allow enumeration of MoltenVK portability devices */
    r->portability_enumeration_extension_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#else
    r->portability_enumeration_extension_enabled = false;
#endif
}

#ifndef _WIN32
typedef VkResult(VKAPI_PTR *IcdNegotiateFn)(uint32_t *version);

/*
 * Loads a Vulkan driver from a dylib path in dlopen() syntax and calls it
 * directly, with no Vulkan loader in between: through its
 * vkGetInstanceProcAddr when it exports one (MoltenVK, a loader),
 * otherwise through the loader-ICD entry vk_icdGetInstanceProcAddr after
 * the interface negotiation a loader would do (Mesa KosmicKrisp exports
 * only the vk_icd* entry points). The module is never closed, like the one
 * volkInitialize() opens: KosmicKrisp registers Objective-C classes and
 * atexit handlers.
 */
static bool load_vk_driver(const char *path, Error **errp)
{
    void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        error_setg(errp, "dlopen: %s", dlerror());
        return false;
    }

    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr");
    if (!gipa) {
        IcdNegotiateFn negotiate = (IcdNegotiateFn)dlsym(
            module, "vk_icdNegotiateLoaderICDInterfaceVersion");
        uint32_t version = 7;
        if (!negotiate || negotiate(&version) != VK_SUCCESS || version < 5) {
            error_setg(errp, "%s: no loader-ICD interface 5 or later", path);
            return false;
        }
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(module,
                                                "vk_icdGetInstanceProcAddr");
    }
    if (!gipa) {
        error_setg(errp, "%s exports no vkGetInstanceProcAddr", path);
        return false;
    }

    volkInitializeCustom(gipa);
    return true;
}
#endif

#ifdef VK_API_VERSION_1_4
#define MAX_VK_API_VERSION VK_API_VERSION_1_4
#else
#define MAX_VK_API_VERSION VK_API_VERSION_1_3
#endif

/* path: the driver's dylib (load_vk_driver), or NULL for volkInitialize(). */
static bool create_instance(PGRAPHState *pg, const char *path, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

#ifndef _WIN32
    if (path) {
        if (!load_vk_driver(path, errp)) {
            return false;
        }
    } else
#endif
    {
        result = volkInitialize();
        if (result != VK_SUCCESS) {
            error_setg(errp, "volkInitialize failed");
            return false;
        }
    }

    uint32_t instance_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        vkEnumerateInstanceVersion(&instance_version);
        instance_version = MIN(instance_version, MAX_VK_API_VERSION);
    }
    if (instance_version < VK_API_VERSION_1_1) {
        error_setg(errp, "Vulkan 1.1 or higher is required");
        return false;
    }
    r->vk_api_version = instance_version;

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xemu",
        .applicationVersion = VK_MAKE_VERSION(
            xemu_version_major, xemu_version_minor, xemu_version_patch),
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = r->vk_api_version,
    };

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_instance_extensions(pg);
    if (!available_extensions) {
        error_setg(errp, "Failed to enumerate instance extensions");
        return false;
    }

    g_autoptr(StringArray) enabled_extension_names =
        g_array_new(FALSE, FALSE, sizeof(char *));

    add_optional_instance_extension_names(pg, available_extensions,
                                          enabled_extension_names);

    if (enabled_extension_names->len > 0) {
        fprintf(stderr, "Enabled instance extensions:\n");
        for (int i = 0; i < enabled_extension_names->len; i++) {
            fprintf(stderr, "- %s\n",
                    g_array_index(enabled_extension_names, char *, i));
        }
    }

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames =
            &g_array_index(enabled_extension_names, const char *, 0),
    };
#ifdef __APPLE__
    if (r->portability_enumeration_extension_enabled) {
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    bool enable_validation = g_config.display.vulkan.validation_layers;

    VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        // VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
    };

    VkValidationFeaturesEXT validationFeatures = {
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = ARRAY_SIZE(enables),
        .pEnabledValidationFeatures = enables,
    };

    if (enable_validation) {
        if (check_validation_layer_support()) {
            fprintf(stderr, "Warning: Validation layers enabled. Expect "
                            "performance impact.\n");
            create_info.enabledLayerCount = ARRAY_SIZE(validation_layers);
            create_info.ppEnabledLayerNames = validation_layers;
            create_info.pNext = &validationFeatures;
        } else {
            fprintf(stderr, "Warning: validation layers not available\n");
        }
    }

    result = vkCreateInstance(&create_info, NULL, &r->instance);
    if (result != VK_SUCCESS) {
        error_setg(errp, "Failed to create instance (%d)", result);
        return false;
    }

    volkLoadInstance(r->instance);

    if (r->debug_utils_extension_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };
        result = vkCreateDebugUtilsMessengerEXT(r->instance, &messenger_info,
                                                NULL, &r->debug_messenger);
        if (result != VK_SUCCESS) {
            r->debug_messenger = VK_NULL_HANDLE;
            error_setg(errp, "Failed to create debug messenger (%d)", result);
            return false;
        }
    }

    return true;
}

static bool is_queue_family_indicies_complete(QueueFamilyIndices indices)
{
    return indices.queue_family >= 0;
}

QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device)
{
    QueueFamilyIndices indices = {
        .queue_family = -1,
    };

    uint32_t num_queue_families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families, NULL);

    g_autofree VkQueueFamilyProperties *queue_families =
        g_malloc_n(num_queue_families, sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &num_queue_families,
                                             queue_families);

    for (int i = 0; i < num_queue_families; i++) {
        VkQueueFamilyProperties queueFamily = queue_families[i];
        int required_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((queueFamily.queueFlags & required_flags) == required_flags) {
            indices.queue_family = i;
        }
        if (is_queue_family_indicies_complete(indices)) {
            break;
        }
    }

    return indices;
}

static VkExtensionPropertiesArray *
get_available_device_extensions(VkPhysicalDevice device)
{
    uint32_t num_extensions = 0;

    if (vkEnumerateDeviceExtensionProperties(device, NULL, &num_extensions,
                                             NULL) != VK_SUCCESS) {
        return NULL;
    }

    VkExtensionPropertiesArray *extensions = g_array_sized_new(
        FALSE, FALSE, sizeof(VkExtensionProperties), num_extensions);

    g_array_set_size(extensions, num_extensions);
    if (vkEnumerateDeviceExtensionProperties(
            device, NULL, &num_extensions,
            (VkExtensionProperties *)extensions->data) != VK_SUCCESS) {
        g_array_free(extensions, TRUE);
        return NULL;
    }

    return extensions;
}

static StringArray *get_required_device_extension_names(void)
{
    StringArray *extensions =
        g_array_sized_new(FALSE, FALSE, sizeof(char *),
                          ARRAY_SIZE(required_device_extensions));

    g_array_append_vals(extensions, required_device_extensions,
                        ARRAY_SIZE(required_device_extensions));

    return extensions;
}

static void add_optional_device_extension_names(
    PGRAPHState *pg, VkExtensionPropertiesArray *available_extensions,
    StringArray *enabled_extension_names)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /*
     * The spec requires enabling VK_KHR_portability_subset whenever the
     * device advertises it. First, so MoltenVK's list keeps the order it
     * had while the extension was required.
     */
    if (is_extension_available(available_extensions,
                               VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        const char *name = VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
        g_array_append_val(enabled_extension_names, name);
    }

    r->custom_border_color_extension_enabled =
        add_extension_if_available(available_extensions, enabled_extension_names,
                                   VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);

    r->memory_budget_extension_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    /* Lets the pipeline cache save only when it gained entries (draw.c). */
    r->pipeline_creation_feedback_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);

#if HAVE_IOSURFACE_SHARING
    r->metal_objects_extension_enabled = add_extension_if_available(
        available_extensions, enabled_extension_names,
        VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
    /*
     * Without metal_objects (stock Mesa KosmicKrisp),
     * VK_EXT_external_memory_metal's MTLHeap export still gets the
     * compositor output to Metal (copy-present, display.c).
     */
    r->metal_copy_present =
        !r->metal_objects_extension_enabled &&
        add_extension_if_available(available_extensions,
                                   enabled_extension_names,
                                   VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME);
#endif
}

static bool check_device_support_required_extensions(VkPhysicalDevice device)
{
    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(device);
    if (!available_extensions) {
        return false;
    }

    for (int i = 0; i < ARRAY_SIZE(required_device_extensions); i++) {
        if (!is_extension_available(available_extensions,
                                    required_device_extensions[i])) {
            fprintf(stderr, "required device extension not found: %s\n",
                    required_device_extensions[i]);
            return false;
        }
    }

    return true;
}

static bool is_device_compatible(VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) {
        return false;
    }

    QueueFamilyIndices indices = pgraph_vk_find_queue_families(device);

    return is_queue_family_indicies_complete(indices) &&
           check_device_support_required_extensions(device);
    // FIXME: Check formats
    // FIXME: Check vram
}

static bool select_physical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    uint32_t num_physical_devices = 0;

    result =
        vkEnumeratePhysicalDevices(r->instance, &num_physical_devices, NULL);
    if (result != VK_SUCCESS || num_physical_devices == 0) {
        error_setg(errp, "Failed to find GPUs with Vulkan support");
        return false;
    }

    g_autofree VkPhysicalDevice *devices =
        g_malloc_n(num_physical_devices, sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(r->instance, &num_physical_devices, devices);

    const char *preferred_device = g_config.display.vulkan.preferred_physical_device;
    int preferred_device_index = -1;

    fprintf(stderr, "Available physical devices:\n");
    for (int i = 0; i < num_physical_devices; i++) {
        vkGetPhysicalDeviceProperties(devices[i], &r->device_props);
        bool is_preferred =
            preferred_device &&
            !strcmp(r->device_props.deviceName, preferred_device);
        if (is_preferred) {
            preferred_device_index = i;
        }
        fprintf(stderr, "- %s%s\n", r->device_props.deviceName,
                is_preferred ? " *" : "");
    }

    r->physical_device = VK_NULL_HANDLE;

    if (preferred_device_index >= 0 &&
        is_device_compatible(devices[preferred_device_index])) {
        r->physical_device = devices[preferred_device_index];
    } else {
        for (int i = 0; i < num_physical_devices; i++) {
            if (is_device_compatible(devices[i])) {
                r->physical_device = devices[i];
                break;
            }
        }
    }
    if (r->physical_device == VK_NULL_HANDLE) {
        error_setg(errp, "Failed to find a suitable GPU");
        return false;
    }

    vkGetPhysicalDeviceProperties(r->physical_device, &r->device_props);
    r->vk_api_version = MIN(r->vk_api_version, r->device_props.apiVersion);

    fprintf(stderr,
            "Selected physical device: %s\n"
            "- Vendor: %x, Device: %x\n"
            "- Driver Version: %d.%d.%d\n",
            r->device_props.deviceName,
            r->device_props.vendorID,
            r->device_props.deviceID,
            VK_VERSION_MAJOR(r->device_props.driverVersion),
            VK_VERSION_MINOR(r->device_props.driverVersion),
            VK_VERSION_PATCH(r->device_props.driverVersion));

    /*
     * Which driver, for logs and bench receipts ("- Driver: MoltenVK,
     * 1.4.3 (id 14)", printed once the device is accepted) and for the few
     * paths that depend on one driver's behaviour rather than on the
     * Vulkan spec: those test driver_id, never display.vulkan.driver. Core
     * in 1.2 (vk_api_version is already capped by both the instance and
     * the device).
     */
    r->driver_id = 0;
    snprintf(r->driver_desc, sizeof(r->driver_desc), "%s",
             r->device_props.deviceName);
    if (r->vk_api_version >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceDriverProperties driver_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 props2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver_props,
        };
        vkGetPhysicalDeviceProperties2(r->physical_device, &props2);
        r->driver_id = driver_props.driverID;
        snprintf(r->driver_desc, sizeof(r->driver_desc), "%s, %s",
                 driver_props.driverName, driver_props.driverInfo);
        /* MoltenVK caches shader stages, never whole pipelines (draw.c). */
        r->pipeline_cache_stage_hits_suffice =
            driver_props.driverID == VK_DRIVER_ID_MOLTENVK;
    }
    r->driver_is_moltenvk = r->driver_id == VK_DRIVER_ID_MOLTENVK;

    return true;
}

static bool create_logical_device(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    g_autoptr(VkExtensionPropertiesArray) available_extensions =
        get_available_device_extensions(r->physical_device);
    if (!available_extensions) {
        error_setg(errp, "Failed to enumerate device extensions");
        return false;
    }

    g_autoptr(StringArray) enabled_extension_names =
        get_required_device_extension_names();

    add_optional_device_extension_names(pg, available_extensions,
                                        enabled_extension_names);

    fprintf(stderr, "Enabled device extensions:\n");
    for (int i = 0; i < enabled_extension_names->len; i++) {
        fprintf(stderr, "- %s\n",
                g_array_index(enabled_extension_names, char *, i));
    }

    /*
     * Single queue. A second compute queue was acquired here at one
     * point but never submitted to (MoltenVK exposes queueCount=1
     * anyway; see docs/optimizations.md "Failed / reverted
     * experiments": separate compute queue).
     */
    float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = indices.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    // Check device features
    VkPhysicalDeviceFeatures physical_device_features;
    vkGetPhysicalDeviceFeatures(r->physical_device, &physical_device_features);
    memset(&r->enabled_physical_device_features, 0,
           sizeof(r->enabled_physical_device_features));

    struct {
        const char *name;
        VkBool32 available, *enabled;
        bool required;
    } desired_features[] = {
        // clang-format off
        #define F(n, req) { \
            .name = #n, \
            .available = physical_device_features.n, \
            .enabled = &r->enabled_physical_device_features.n, \
            .required = req, \
        }
        // macOS (Metal/MoltenVK) lacks native support for geometry shaders and
        // some other strict features. Relaxing these requirements here enables
        // Vulkan to initialize on Apple Silicon.
#ifdef __APPLE__
// TODO: MoltenVK Fix: change this when there's a better solution for MoltenVK.
        F(depthClamp, true),
        F(fillModeNonSolid, false),
        F(geometryShader, false),
        F(occlusionQueryPrecise, false),
        F(samplerAnisotropy, true),
        F(shaderClipDistance, true),
        F(shaderTessellationAndGeometryPointSize, false),
        F(wideLines, false),
#else
        F(depthClamp, true),
        F(fillModeNonSolid, true),
        F(geometryShader, true),
        F(occlusionQueryPrecise, true),
        F(samplerAnisotropy, false),
        F(shaderClipDistance, true),
        /*
         * Optional: only governs writing gl_PointSize from geometry/tess
         * shaders. Enabled when the driver has it (no fidelity change on
         * capable GPUs), but not required — some Windows Vulkan ICDs
         * (paravirtual GPUs in VMs, weak iGPUs, D3D12-backed layers) lack
         * it and would otherwise hard-abort device init. Apple already
         * runs with this off.
         */
        F(shaderTessellationAndGeometryPointSize, false),
        F(wideLines, false),
#endif
        #undef F
        // clang-format on
    };

    bool all_required_features_available = true;
    for (int i = 0; i < ARRAY_SIZE(desired_features); i++) {
        if (desired_features[i].required &&
            desired_features[i].available != VK_TRUE) {
            fprintf(stderr,
                    "Error: Device does not support required feature %s\n",
                    desired_features[i].name);
            all_required_features_available = false;
        }
        *desired_features[i].enabled = desired_features[i].available;
    }

    if (!all_required_features_available) {
        error_setg(errp, "Device does not support required features");
        return false;
    }
    r->supports_geometry_shaders =
        r->enabled_physical_device_features.geometryShader == VK_TRUE;

    /*
     * Metal has no triangle fans, so every Apple driver gets CPU-built
     * triangle lists. MoltenVK would convert each fan draw in a compute
     * encoder that restarts the render pass (heavy-f1: 101 Metal encoders
     * a flip instead of 38); Mesa KosmicKrisp unrolls it in a compute pass
     * that ends the render encoder too and, as of Mesa 27.0-devel
     * 335d5698dc, mangles some of them (Azurik's HUD gauges in
     * azurik-heavy-w2 / -stream-e2).
     */
#ifdef __APPLE__
    r->emulate_triangle_fans = true;
#else
    r->emulate_triangle_fans = false;
#endif

    /*
     * Whether a geometry shader may write gl_PointSize. Emitting that write
     * makes glslang declare the GeometryPointSize SPIR-V capability, which
     * requires this feature; if it's unavailable the GS pipeline would fail
     * capability validation. Thread this into GS generation so the write is
     * omitted when unsupported (see pgraph_glsl_gen_geom).
     */
    r->supports_geom_point_size =
        r->enabled_physical_device_features.shaderTessellationAndGeometryPointSize ==
        VK_TRUE;

    void *next_struct = NULL;

    /*
     * Timeline semaphores (Vulkan 1.2 core) back the async compositor
     * submit on the Metal presentation backend: MoltenVK implements
     * them with MTLSharedEvent, which VK_EXT_metal_objects can export
     * for GPU-side cross-API waits. Optional; probed here and at
     * export time, with fallback to the synchronous aux fence.
     */
    r->timeline_semaphore_enabled = false;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    };
    if (r->vk_api_version >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceTimelineSemaphoreFeatures query = {
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &query,
        };
        vkGetPhysicalDeviceFeatures2(r->physical_device, &features2);
        if (query.timelineSemaphore == VK_TRUE) {
            timeline_features.timelineSemaphore = VK_TRUE;
            timeline_features.pNext = next_struct;
            next_struct = &timeline_features;
            r->timeline_semaphore_enabled = true;
        }
    }

    /*
     * On Vulkan 1.3 and later glsl.c compiles for SPIR-V 1.6
     * (glsl_target_for), where glslang lowers discard to
     * OpDemoteToHelperInvocation, and that capability needs this feature
     * enabled: without it the validation layer flags every fragment
     * shader (MoltenVK and KosmicKrisp alike). Core in 1.3; enabled only
     * when the device reports it.
     */
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote_features = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
    };
    if (r->vk_api_version >= VK_API_VERSION_1_3) {
        VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures query = {
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &query,
        };
        vkGetPhysicalDeviceFeatures2(r->physical_device, &features2);
        if (query.shaderDemoteToHelperInvocation == VK_TRUE) {
            demote_features.shaderDemoteToHelperInvocation = VK_TRUE;
            demote_features.pNext = next_struct;
            next_struct = &demote_features;
        }
    }

    VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border_features;
    if (r->custom_border_color_extension_enabled) {
        custom_border_features = (VkPhysicalDeviceCustomBorderColorFeaturesEXT){
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
            .customBorderColors = VK_TRUE,
            .pNext = next_struct,
        };
        next_struct = &custom_border_features;
    }

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create_info,
        .pEnabledFeatures = &r->enabled_physical_device_features,
        .enabledExtensionCount = enabled_extension_names->len,
        .ppEnabledExtensionNames =
            &g_array_index(enabled_extension_names, const char *, 0),
        .pNext = next_struct,
    };

    result = vkCreateDevice(r->physical_device, &device_create_info, NULL,
                            &r->device);
    if (result != VK_SUCCESS) {
        r->device = VK_NULL_HANDLE;
        error_setg(errp, "Failed to create logical device (%d)", result);
        return false;
    }
#if HAVE_IOSURFACE_SHARING
    r->get_memory_metal_handle_fn =
        r->metal_copy_present ?
            (void *)vkGetDeviceProcAddr(r->device, "vkGetMemoryMetalHandleEXT") :
            NULL;
#endif

    /*
     * With volkLoadInstance() alone, every device-level entry point
     * (vkCmdDraw, vkCmdBindDescriptorSets, ...) is the pointer
     * vkGetInstanceProcAddr returned, i.e. the Vulkan loader's trampoline,
     * which re-derives the ICD dispatch table from the handle on every call.
     * volkLoadDevice() replaces the globals with vkGetDeviceProcAddr()
     * results that enter the ICD directly. Hygiene, not a measured win.
     * Safe here because the renderer creates exactly one VkDevice; a second
     * device would need volkLoadDeviceTable() instead. Entry points of
     * device extensions not enabled above become NULL rather than
     * trampolines, which no call site can hit: the only extension entry
     * points the tree calls through volk are vkGetMemoryWin32HandleKHR /
     * vkGetMemoryFdKHR, whose extensions are required above; volk keeps
     * VK_EXT_debug_utils in its instance loader, so the label/object-name
     * calls retain their volkLoadInstance pointers; vkExportMetalObjectsEXT
     * is resolved separately via vkGetDeviceProcAddr; and VMA builds its own
     * table with volkLoadDeviceTable() regardless.
     *
     * Skipped for MoltenVK, which is loaded as the ICD with no loader
     * trampoline to bypass, so its dispatch stays exactly as
     * volkLoadInstance() left it. Not for other Apple drivers: Mesa
     * KosmicKrisp hands out per-device trampolines through
     * vk_icdGetInstanceProcAddr.
     */
#ifdef __APPLE__
    if (!r->driver_is_moltenvk)
#endif
    {
        volkLoadDevice(r->device);
    }

    vkGetDeviceQueue(r->device, indices.queue_family, 0, &r->queue);

    return true;
}

uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPhysicalDeviceMemoryProperties prop;
    vkGetPhysicalDeviceMemoryProperties(r->physical_device, &prop);
    for (uint32_t i = 0; i < prop.memoryTypeCount; i++) {
        if ((prop.memoryTypes[i].propertyFlags & properties) == properties &&
            type_bits & (1 << i)) {
            return i;
        }
    }
    return 0xFFFFFFFF; // Unable to find memoryType
}

static bool init_allocator(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    VkResult result;

    VmaAllocatorCreateInfo create_info = {
        .flags = (r->memory_budget_extension_enabled ?
                      VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT :
                      0),
        .vulkanApiVersion = r->vk_api_version,
        .instance = r->instance,
        .physicalDevice = r->physical_device,
        .device = r->device,
    };

    VmaVulkanFunctions vulkan_functions;
    VkResult res = vmaImportVulkanFunctionsFromVolk(&create_info, &vulkan_functions);
    if (res != VK_SUCCESS) {
        error_setg(errp, "vmaImportVulkanFunctionsFromVolk failed");
        return false;
    }
    create_info.pVulkanFunctions = &vulkan_functions;

    result = vmaCreateAllocator(&create_info, &r->allocator);
    if (result != VK_SUCCESS) {
        error_setg(errp, "vmaCreateAllocator failed");
        return false;
    }

    return true;
}

/*
 * The Vulkan driver, chosen at the first renderer init and kept for the
 * process (a later init, after a renderer switch, loads the same one):
 *
 * 1. XEMU_VK_DRIVER (diagnostic; POSIX): "kosmickrisp", "moltenvk" (the
 *    copies bundled beside the other dylibs) or a dylib path. Overrides
 *    the setting and the pin; if it fails, xemu exits rather than let
 *    pgraph.c persist renderer = OPENGL.
 * 2. Not Apple: volkInitialize(), the system loader.
 * 3. Apple: MoltenVK, unless display.vulkan.driver is kosmickrisp (an
 *    opt-in; Auto runs MoltenVK); then KosmicKrisp unless
 *    nv2a_vk_kk_ineligible_reason() names a reason (the pin among them)
 *    or a strike names this build, and it must pass kk_accept(), else
 *    MoltenVK runs.
 *    MoltenVK loads by its bundled path, never volk's search, so a Vulkan
 *    loader in /usr/local can't take its place.
 *
 * Paths that depend on one driver's behaviour test r->driver_id, never
 * the setting.
 */
static struct {
    bool resolved;
    int setting;        /* display.vulkan.driver as resolve_driver read it */
    const char *path;   /* what this process loads; NULL: volkInitialize() */
    bool kk_strikable;  /* KosmicKrisp in use, chosen by the setting */
    bool start_open;    /* the start window (strike marker, below) is open */
    int start_flips;    /* guest flips since it opened (PFIFO thread) */
    char start_tag[24]; /* " (pid N)", set before start_open */
    char *desc;         /* nv2a_vk_driver_desc() */
} vk_driver;

#ifdef __APPLE__
#define KK_DYLIB "@rpath/libvulkan_kosmickrisp.dylib"
#define MVK_DYLIB "@rpath/libMoltenVK.dylib"

/*
 * What KosmicKrisp must show before xemu commits to it; past this point a
 * failure can only end the process. Copy-present (display.c) is its only
 * present path: VK_EXT_external_memory_metal, vkGetMemoryMetalHandleEXT,
 * timeline semaphores (the forwarder) and one small target, made and
 * freed. Every surface and texture format, which would otherwise abort
 * later in vkCreateImage.
 */
static bool kk_accept(PGRAPHState *pg, Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->metal_copy_present || !r->get_memory_metal_handle_fn) {
        error_setg(errp, "no VK_EXT_external_memory_metal");
        return false;
    }
    if (!r->timeline_semaphore_enabled) {
        error_setg(errp, "no timeline semaphores");
        return false;
    }
    return pgraph_vk_formats_supported(r, errp) &&
           pgraph_vk_copy_present_probe(pg, errp);
}

const char *nv2a_vk_kk_ineligible_reason(void)
{
#ifndef __aarch64__
    return "needs Apple silicon";
#else
    if (icount_enabled()) {
        return "pinned: deterministic";
    }
    if (xemu_headless_mode() == XEMU_HEADLESS_OFFSCREEN) {
        return "pinned: offscreen";
    }
    if (!xemu_present_is_metal()) {
        return "needs Metal presentation";
    }
    if (__builtin_available(macOS 26.0, *)) {
        return metalfx_device_is_metal4() ? NULL : "needs a Metal 4 GPU";
    }
    return "needs macOS 26";
#endif
}

/*
 * The strike marker, <base>kosmickrisp-strike.txt: the build it applies to
 * (kk_build_id), then why. A launch that tries KosmicKrisp opens a start
 * window: it writes "crashed while starting" before the attempt (after a
 * clean refusal with the dylib loaded, "MoltenVK crashed after a
 * KosmicKrisp attempt" before MoltenVK starts), tagged " (pid N)". Only
 * that process removes its tagged line, after KK_START_FLIPS guest flips,
 * at a clean exit or at renderer teardown, so a crash in the driver's
 * start or first frames (a signal, a Mesa abort, a Metal assertion) leaves
 * it. A fatal error writes its reason (pgraph_vk_driver_strike), tagged
 * while the window is open.
 *
 * A line that names the running build keeps later launches on MoltenVK,
 * so a KosmicKrisp that fails can't take every launch down with it; a
 * tagged line whose pid still runs belongs to another xemu on the same
 * data folder and doesn't count (this launch's start line then replaces
 * it). Picking KosmicKrisp in Settings removes the marker
 * (nv2a_vk_driver_retry), so each pick gets one more attempt.
 */
#define KK_STARTING "crashed while starting"
#define KK_MVK_AFTER "MoltenVK crashed after a KosmicKrisp attempt"
#define KK_START_FLIPS 600 /* about 10 s of play */

static char *kk_strike_path(void)
{
    return g_strdup_printf("%skosmickrisp-strike.txt",
                           xemu_settings_get_base_path());
}

/* The xemu version and the bundled dylib's size and mtime. */
static char *kk_build_id(void)
{
    char exe[PATH_MAX];
    uint32_t len = sizeof(exe);
    struct stat st;
    if (_NSGetExecutablePath(exe, &len) == 0) {
        g_autofree char *dir = g_path_get_dirname(exe);
        g_autofree char *dylib = g_build_filename(
            dir, "../Libraries/arm64/libvulkan_kosmickrisp.dylib", NULL);
        if (stat(dylib, &st) == 0) {
            return g_strdup_printf("xemu %s, KosmicKrisp %lld bytes, mtime %lld",
                                   xemu_version, (long long)st.st_size,
                                   (long long)st.st_mtime);
        }
    }
    return g_strdup_printf("xemu %s, no bundled KosmicKrisp", xemu_version);
}

static void kk_strike_write(const char *reason)
{
    g_autofree char *path = kk_strike_path();
    g_autofree char *id = kk_build_id();
    g_autofree char *text = g_strdup_printf(
        "%s\n%s%s\n", id, reason,
        qatomic_read(&vk_driver.start_open) ? vk_driver.start_tag : "");
    g_file_set_contents(path, text, -1, NULL);
}

/*
 * The marker's reason, without its tag, if it names this build, else
 * NULL; *pid from the tag (0 without one).
 */
static char *kk_strike_load(int *pid)
{
    g_autofree char *path = kk_strike_path();
    g_autofree char *id = kk_build_id();
    g_autofree char *text = NULL;
    *pid = 0;
    if (!g_file_get_contents(path, &text, NULL, NULL) ||
        !g_str_has_prefix(text, id) || text[strlen(id)] != '\n') {
        return NULL;
    }
    char *why = g_strstrip(g_strdup(text + strlen(id) + 1));
    char *tag = g_strrstr(why, " (pid ");
    int end = 0;
    if (tag && sscanf(tag, " (pid %d)%n", pid, &end) == 1 && !tag[end]) {
        *tag = '\0';
    } else {
        *pid = 0;
    }
    return why;
}

static bool pid_alive(int pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

/* Why KosmicKrisp shouldn't start in this launch, or NULL (above). */
static char *kk_strike_read(void)
{
    int pid;
    char *why = kk_strike_load(&pid);
    if (why && pid != getpid() && pid_alive(pid)) {
        g_clear_pointer(&why, g_free);
    }
    return why;
}

/* Close the start window: remove the marker if it is still our line. */
static void kk_start_window_close(void)
{
    if (!qatomic_xchg(&vk_driver.start_open, false)) {
        return;
    }
    int pid;
    g_autofree char *why = kk_strike_load(&pid);
    if (why && pid == getpid() &&
        (g_str_has_prefix(why, KK_STARTING) ||
         g_str_has_prefix(why, KK_MVK_AFTER))) {
        g_autofree char *path = kk_strike_path();
        g_unlink(path);
    }
}

static void kk_start_window_open(void)
{
    snprintf(vk_driver.start_tag, sizeof(vk_driver.start_tag), " (pid %d)",
             (int)getpid());
    qatomic_set(&vk_driver.start_open, true);
    /* A quit or SIGTERM is no crash; SIGKILL still counts as one */
    atexit(kk_start_window_close);
    kk_strike_write(KK_STARTING);
}

/* A toast, in the log too (only headless notifications are). */
static void kk_notify(const char *msg)
{
    if (!xemu_headless_mode()) {
        fprintf(stderr, "xemu: notification: %s\n", msg);
    }
    xemu_queue_notification(msg);
}
#endif

void pgraph_vk_driver_strike(const char *reason)
{
#ifdef __APPLE__
    /* The window also covers MoltenVK's first frames after a KK attempt */
    if (vk_driver.kk_strikable || qatomic_read(&vk_driver.start_open)) {
        kk_strike_write(reason);
    }
#endif
}

void pgraph_vk_driver_flip(void)
{
#ifdef __APPLE__
    if (unlikely(qatomic_read(&vk_driver.start_open)) &&
        ++vk_driver.start_flips >= KK_START_FLIPS) {
        kk_start_window_close();
    }
#endif
}

void pgraph_vk_driver_stopped(void)
{
#ifdef __APPLE__
    kk_start_window_close();
#endif
}

bool nv2a_vk_driver_retry(void)
{
#ifdef __APPLE__
    int pid;
    g_autofree char *why = kk_strike_load(&pid);
    /* A live process's start line still guards that launch */
    if (why && !pid_alive(pid)) {
        g_autofree char *path = kk_strike_path();
        return g_unlink(path) == 0;
    }
#endif
    return false;
}

const char *nv2a_vk_driver_desc(void)
{
    return qatomic_load_acquire(&vk_driver.desc);
}

int nv2a_vk_driver_setting(void)
{
    /* Written before desc's release store */
    return nv2a_vk_driver_desc() ? vk_driver.setting : -1;
}

/* One driver, instance to allocator; a failure leaves r as it found it. */
static bool init_on(PGRAPHState *pg, const char *path, bool want_kk,
                    Error **errp)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    bool ok = create_instance(pg, path, errp) &&
              select_physical_device(pg, errp);
#ifdef __APPLE__
    if (ok && want_kk && r->driver_id != VK_DRIVER_ID_MESA_KOSMICKRISP) {
        error_setg(errp, "not a KosmicKrisp device (driver id %d)",
                   r->driver_id);
        ok = false;
    }
    ok = ok && create_logical_device(pg, errp) &&
         (r->driver_id != VK_DRIVER_ID_MESA_KOSMICKRISP ||
          kk_accept(pg, errp));
#else
    ok = ok && create_logical_device(pg, errp);
#endif
    ok = ok && init_allocator(pg, errp);
    if (!ok) {
        /* Nothing writes r before this init (renderer.c) */
        pgraph_vk_finalize_instance(pg);
        memset(r, 0, sizeof(*r));
    }
    return ok;
}

/* why: the resolver line's reason, NULL on later inits and off Apple */
static void driver_accepted(PGRAPHState *pg, const char *why)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (why) {
        const char *name = r->driver_desc;
#ifdef __APPLE__
        if (r->driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP) {
            name = "KosmicKrisp";
        } else if (r->driver_is_moltenvk) {
            name = "MoltenVK";
        }
#endif
        fprintf(stderr, "xemu: Vulkan driver: %s (%s)\n", name, why);
    }
    fprintf(stderr, "- Driver: %s (id %d)\n", r->driver_desc, r->driver_id);
    xemu_settings_set_string(&g_config.display.vulkan.preferred_physical_device,
                             r->device_props.deviceName);
    if (!vk_driver.desc) {
        qatomic_store_release(&vk_driver.desc,
                              why ? g_strdup_printf("%s (%s)", r->driver_desc,
                                                    why) :
                                    g_strdup(r->driver_desc));
    }
}

static bool resolve_driver(PGRAPHState *pg, Error **errp)
{
    vk_driver.setting = g_config.display.vulkan.driver;
#ifndef _WIN32
    const char *env = getenv("XEMU_VK_DRIVER");
    if (env && *env) {
        const char *path = env;
#ifdef __APPLE__
        path = !strcmp(env, "kosmickrisp") ? KK_DYLIB :
               !strcmp(env, "moltenvk")    ? MVK_DYLIB :
                                             env;
#endif
        vk_driver.path = g_strdup(path);
        if (!init_on(pg, vk_driver.path, false, errp)) {
            fprintf(stderr, "xemu: XEMU_VK_DRIVER=%s: %s\n", env,
                    error_get_pretty(*errp));
            exit(1);
        }
        g_autofree char *why = g_strdup_printf("XEMU_VK_DRIVER=%s", env);
        driver_accepted(pg, why);
        return true;
    }
#endif

#ifndef __APPLE__
    if (!init_on(pg, NULL, false, errp)) {
        return false;
    }
    driver_accepted(pg, NULL);
    return true;
#else
    static const char *const setting_name[] = {
        [CONFIG_DISPLAY_VULKAN_DRIVER_AUTO] = "auto",
        [CONFIG_DISPLAY_VULKAN_DRIVER_KOSMICKRISP] = "kosmickrisp",
        [CONFIG_DISPLAY_VULKAN_DRIVER_MOLTENVK] = "moltenvk",
    };
    int setting = vk_driver.setting;
    /*
     * KosmicKrisp is an opt-in: Auto runs MoltenVK. Auto may try
     * KosmicKrisp again once it passes the Auto-back gate
     * (docs/kosmickrisp.md, What flips Auto back); that is this one line.
     */
    bool try_kk = setting == CONFIG_DISPLAY_VULKAN_DRIVER_KOSMICKRISP;
    const char *why =
        try_kk ? nv2a_vk_kk_ineligible_reason() : setting_name[setting];
    g_autofree char *why_buf = NULL;
    g_autofree char *strike = NULL;
    bool kk_loaded = false;

    if (!why && (strike = kk_strike_read())) {
        why = why_buf = g_strdup_printf(
            "KosmicKrisp failed in an earlier launch: %s", strike);
        kk_notify("KosmicKrisp failed in an earlier launch; using MoltenVK. "
                  "Pick KosmicKrisp in Settings > Display to retry.");
    } else {
        if (!why) {
            Error *kk_err = NULL;
            kk_start_window_open();
            if (init_on(pg, KK_DYLIB, true, &kk_err)) {
                vk_driver.path = KK_DYLIB;
                vk_driver.kk_strikable = true;
                driver_accepted(pg, setting_name[setting]);
                return true;
            }
            why = "KosmicKrisp unavailable";
            why_buf = g_strdup_printf("%s (%s)", why, error_get_pretty(kk_err));
            error_free(kk_err);
            fprintf(stderr, "xemu: %s; using MoltenVK\n", why_buf);
            /*
             * A dylib that loaded stays loaded (never closed), so MoltenVK
             * shares the process with it: the window then covers
             * MoltenVK's start and first frames. Otherwise MoltenVK runs
             * alone, as with the setting at moltenvk.
             */
            void *kk = dlopen(KK_DYLIB, RTLD_NOLOAD);
            kk_loaded = kk != NULL;
            if (kk_loaded) {
                dlclose(kk);
                kk_strike_write(KK_MVK_AFTER);
            } else {
                kk_start_window_close();
            }
        }
#ifdef __aarch64__
        if (setting == CONFIG_DISPLAY_VULKAN_DRIVER_KOSMICKRISP) {
            g_autofree char *msg = g_strdup_printf(
                "%s%s; using MoltenVK", why_buf ? "" : "KosmicKrisp: ",
                why_buf ? why_buf : why);
            kk_notify(msg);
        }
#endif
    }

    vk_driver.path = MVK_DYLIB;
    if (!init_on(pg, MVK_DYLIB, false, errp)) {
        if (kk_loaded) {
            /* Not pgraph.c's OPENGL fallback: KosmicKrisp may be to blame. */
            kk_strike_write("MoltenVK failed after a KosmicKrisp attempt");
            qatomic_set(&vk_driver.start_open, false); /* exit keeps it */
            fprintf(stderr, "xemu: MoltenVK after KosmicKrisp: %s\n",
                    error_get_pretty(*errp));
            exit(1);
        }
        return false;
    }
    driver_accepted(pg, why);
    return true;
#endif
}

void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp)
{
    bool ok;
    if (vk_driver.resolved) {
        ok = init_on(pg, vk_driver.path, false, errp);
        if (ok) {
            driver_accepted(pg, NULL);
        }
    } else {
        vk_driver.resolved = true;
        ok = resolve_driver(pg, errp);
    }
    if (ok) {
        return;
    }

    const char *msg = "Failed to initialize Vulkan renderer";
    if (*errp) {
        error_prepend(errp, "%s: ", msg);
    } else {
        error_setg(errp, "%s", msg);
    }
}

void pgraph_vk_finalize_instance(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(r->allocator);
        r->allocator = VK_NULL_HANDLE;
    }

    if (r->device != VK_NULL_HANDLE) {
        vkDestroyDevice(r->device, NULL);
        r->device = VK_NULL_HANDLE;
    }

    if (r->debug_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(r->instance, r->debug_messenger, NULL);
        r->debug_messenger = VK_NULL_HANDLE;
    }

    if (r->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(r->instance, NULL);
        r->instance = VK_NULL_HANDLE;
    }

    volkFinalize();
}
