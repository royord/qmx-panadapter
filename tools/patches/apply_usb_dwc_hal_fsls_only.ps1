<#
.SYNOPSIS
    Lets the USB host run the Tab5's root port as FULL-SPEED ONLY, so a Full-Speed
    device behind a High-Speed hub (the DigiRig Mobile) can enumerate.

.DESCRIPTION
    The ESP32-P4 USB-OTG core is High-Speed capable. ESP-IDF v5.4.4's hub driver
    does not implement a transaction translator, and refuses what needs one:

        E HUB: Connected device is FS, transaction translator (TT) is not supported
        E EXT_PORT: [1:1] Port disabled

    A DigiRig Mobile is a hub with a CM108 (UAC) and a CP2102 (CAT) behind it, both
    Full-Speed. Plugged into a HS root port the hub comes up High-Speed, both
    children connect at FS, and both are disabled - so the radio is never found.
    Bench 2026-10-07: "scanning 1 USB device(s)" - the hub alone.

    The fix upstream is HCFG.FSLSSupp (host presents FS/LS only; the hub then runs
    FS and every child is FS, no TT needed). esp-usb 1.6.0 exposes it as
    usb_host_config_t.fsls_only, but that release needs ESP-IDF >= 5.5.3 and this
    project is pinned to 5.4.4. The low-level helper
    usb_dwc_ll_hcfg_set_fsls_supp_only() already exists in 5.4.4; nothing calls
    it. This patch calls it from usb_dwc_hal_port_toggle_reset() (so a change takes
    effect on the next port reset), driven by g_qmx_usb_fsls_only, which is defined
    in main/util/usb_patch_counters.c and set from the radio profile (KX3 = FS
    only; QMX = stock High-Speed behaviour).

    Edits the pinned IDF install, not the project tree: re-apply per build
    machine. tools/check_patches.py fails the build if it is missing.

    Idempotent.

.HOW TO USE
    powershell -ExecutionPolicy Bypass -File tools/patches/apply_usb_dwc_hal_fsls_only.ps1
#>

$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Host "IDF_PATH not set - run this from an activated ESP-IDF environment." -ForegroundColor Red
    exit 1
}

$target = Join-Path $env:IDF_PATH "components/hal/include/hal/usb_dwc_hal.h"
if (-not (Test-Path $target)) {
    Write-Host "usb_dwc_hal.h not found at:" -ForegroundColor Red
    Write-Host "  $target"
    exit 1
}

$content   = Get-Content -Raw -Path $target
$contentLf = $content -replace "`r`n", "`n"

if ($contentLf.Contains("g_qmx_usb_fsls_only")) {
    Write-Host "Already patched (FS-only root port) - nothing to do." -ForegroundColor Green
    exit 0
}

$original = @'
static inline void usb_dwc_hal_port_toggle_reset(usb_dwc_hal_context_t *hal, bool enable)
{
    usb_dwc_ll_hprt_set_port_reset(hal->dev, enable);
}
'@

$patched = @'
/* PATCHED (qmx-panadapter, 2026-10-07) g_qmx_usb_fsls_only: FS-only root port.
 * Defined in main/util/usb_patch_counters.c (declared extern here so a missing
 * definition cannot fail the link of some other project). When set, the HS core
 * presents Full-Speed only on the next port reset, so a FS device behind a HS
 * hub works without a transaction translator (see the apply script). */
extern volatile uint32_t g_qmx_usb_fsls_only;
static inline void usb_dwc_hal_port_toggle_reset(usb_dwc_hal_context_t *hal, bool enable)
{
    if (enable && hal->constant_config.hsphy_type) {
        if (g_qmx_usb_fsls_only) {
            usb_dwc_ll_hcfg_set_fsls_supp_only(hal->dev);
        } else {
            hal->dev->hcfg_reg.fslssupp = 0;
        }
    }
    usb_dwc_ll_hprt_set_port_reset(hal->dev, enable);
}
'@

$originalLf = $original -replace "`r`n", "`n"
if (-not $contentLf.Contains($originalLf)) {
    Write-Host "Could not find the stock usb_dwc_hal_port_toggle_reset - usb_dwc_hal.h may have changed." -ForegroundColor Red
    Write-Host "Patch by hand and update this script."
    exit 1
}

$result = $contentLf.Replace($originalLf, ($patched -replace "`r`n", "`n"))
[System.IO.File]::WriteAllText($target, $result, (New-Object System.Text.UTF8Encoding $false))

Write-Host "Patched usb_dwc_hal.h: root port can be forced Full-Speed (g_qmx_usb_fsls_only)." -ForegroundColor Green
Write-Host "  $target"
