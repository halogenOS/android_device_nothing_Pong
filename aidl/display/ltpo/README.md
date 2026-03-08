# LTPO Control HAL

AIDL HAL that allows SurfaceFlinger to control LTPO panel refresh rates via
skip frame mode (SFM). The NT37705 panel runs at 120Hz internally — SFM tells
the display controller to skip frames, achieving real power savings at the
hardware level without changing the HWC display mode.

Two operating modes are supported:

- **Idle mode** (`persist.sys.sfm.mode=idle`): drops to 1Hz when content is
  idle, jumps back to 120Hz on any content update. Simple binary behavior.

- **VRR mode** (`persist.sys.sfm.mode=vrr`): matches the content frame rate
  using the full SFM step table (1/10/24/30/60/120Hz). A 24fps video drives
  the panel at 24Hz, a 60fps game at 60Hz, idle screens at 1Hz.

SurfaceFlinger has a null check and silently skips LTPO control if no HAL
service is registered on the device. No build flags needed — AOSP behavior
is the default.

## AIDL Interface

**Module:** `custom.hardware.display.ltpo`
**Package:** `custom.hardware.display.ltpo`
**Interface:** `ILtpoControl`

### Methods

| Method | Description |
|--------|-------------|
| `setEnabled(boolean)` | Enable/disable LTPO control entirely |
| `setTargetHz(int) → int` | Request a target Hz; returns actual Hz from the closest supported step |
| `getCurrentHz() → int` | Returns current effective Hz |
| `getSupportedHz() → int[]` | Returns all supported Hz values |
| `onIdle()` | Convenience: equivalent to `setTargetHz(1)` |
| `onActive()` | Convenience: equivalent to `setTargetHz(120)` |

### SFM Step Table (NT37705)

| Hz | SFM Index | Notes |
|----|-----------|-------|
| 1 | 11 | Deep idle |
| 10 | 10 | Low activity |
| 24 | 9 | Film content |
| 30 | 8 | 30fps video |
| 60 | 7 | Standard content |
| 120 | 16 (disable) | Full refresh, SFM off |

## Porting to a new device

### Prerequisites

1. **LTPO display** — the panel must support variable refresh rates via a
   kernel sysfs interface (skip frame mode, MIPI DSI idle, or similar)
2. **Known sysfs values** — you need to know which values to write for each
   step (check the panel driver source or datasheet)

### Steps

#### 1. Create the HAL implementation

Copy `aidl/display/ltpo/` to your device tree and adapt:

- **`LtpoControl.h`** — Update `kSysfsPath` to your panel's sysfs node,
  and `kSteps[]` to your panel's supported rates and SFM indices:
  ```cpp
  static constexpr const char* kSysfsPath = "/sys/panel_feature/skip_frame_mode";
  static constexpr SfmStep kSteps[] = {
      {1, 11}, {10, 10}, {24, 9}, {30, 8}, {60, 7}, {120, 16},
  };
  ```

- **`LtpoControl.cpp`** — Should work as-is unless your panel needs
  different write semantics

#### 2. Add to device.mk

```makefile
PRODUCT_PACKAGES += \
    custom.hardware.display.ltpo-service.<device>
```

#### 3. SELinux policy

The common sepolicy in `device/custom/sepolicy` already provides the HAL
attribute (`hal_custom_hardware_display_ltpo`), domain type, binder rules,
service type, service context, and SurfaceFlinger client domain.

You only need device-specific rules in your device sepolicy:

**`sepolicy/vendor/hal_custom_hardware_display_ltpo_default.te`**
```
binder_use(hal_custom_hardware_display_ltpo_default)
allow hal_custom_hardware_display_ltpo_default vendor_sysfs_sfm:file rw_file_perms;
allow hal_custom_hardware_display_ltpo_default vendor_sysfs_sfm:dir { open read search getattr };
```

**`sepolicy/vendor/file_contexts`**
```
/vendor/bin/hw/custom\.hardware\.display\.ltpo-service\.<device>    u:object_r:hal_custom_hardware_display_ltpo_default_exec:s0
```

#### 4. Enable smooth display toggle

Ensure the AOSP smooth display toggle is visible — the LTPO mode selector
appears directly below it in Settings and is only shown when the HAL is
declared on the device:

```xml
<!-- In your Settings overlay res/values/config.xml -->
<bool name="config_show_smooth_display">true</bool>
```

## File overview

| File | Purpose |
|------|---------|
| `Android.bp` | Build rules for `custom.hardware.display.ltpo-service.<device>` |
| `LtpoControl.h` | HAL implementation header — panel constants and step table |
| `LtpoControl.cpp` | HAL implementation — sysfs write logic, VRR/idle mode switching |
| `service.cpp` | HAL service main — registers with ServiceManager |
| `custom.hardware.display.ltpo-service.<device>.rc` | init service, started/stopped via property trigger |
| `custom.hardware.display.ltpo-service.<device>.xml` | VINTF manifest fragment |
