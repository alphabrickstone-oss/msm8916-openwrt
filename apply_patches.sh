#!/bin/bash
# apply_patches.sh
# Applies wcn36xx and ath10k-sdio support patches to OpenWrt's mac80211 package.
# Replaces the git patch approach to work across different OpenWrt versions.
# NOTE: Requires GNU sed (Linux only - runs in Docker or GHA).
set -euo pipefail

OPENWRT_DIR="${1:-openwrt}"
ATH_MK="$OPENWRT_DIR/package/kernel/mac80211/ath.mk"

if [ ! -f "$ATH_MK" ]; then
    echo "Error: $ATH_MK not found"
    exit 1
fi

if grep -q '^# MSM8916_OPENWRT_MAC80211_PATCHES$' "$ATH_MK"; then
    echo "mac80211 patches already applied, skipping"
    exit 0
fi

if grep -Eq '^define KernelPackage/(ath10k-sdio|wcn36xx)$' "$ATH_MK"; then
    echo "Error: found a partial MSM8916 mac80211 patch in $ATH_MK" >&2
    exit 1
fi

require_line() {
    if ! grep -Fq -- "$1" "$ATH_MK"; then
        echo "Error: expected mac80211 definition not found: $1" >&2
        exit 1
    fi
}

# Fail explicitly when the pinned OpenWrt source no longer matches these edits.
require_line 'ath10k ath10k-smallbuffers'
require_line 'wil6210 qcom-qmi-helpers'
require_line 'WIL6210_DEBUGFS'
require_line 'config-$(CONFIG_TARGET_ipq40xx) += ATH10K_AHB'
require_line 'config-$(call config_package,ath10k,regular) += ATH10K ATH10K_PCI'
require_line 'DEPENDS+= @PCI_SUPPORT||USB_SUPPORT||TARGET_ath79 +kmod-mac80211'
require_line 'config ATH10K_THERMAL'
require_line 'PACKAGE_kmod-ath10k || PACKAGE_kmod-ath10k-smallbuffers'

echo "Applying mac80211 wcn36xx/ath10k-sdio patches to $ATH_MK..."

# 1. Add ath10k-sdio and wcn36xx to PKG_DRIVERS
sed -i 's/ath10k ath10k-smallbuffers/ath10k ath10k-sdio ath10k-smallbuffers/' "$ATH_MK"
sed -i 's/wil6210 qcom-qmi-helpers/wil6210 wcn36xx qcom-qmi-helpers/' "$ATH_MK"

# 2. Add WCN36XX_DEBUGFS after WIL6210_DEBUGFS
sed -i '/^[[:space:]]*WIL6210_DEBUGFS$/s/WIL6210_DEBUGFS/WIL6210_DEBUGFS \\\n\tWCN36XX_DEBUGFS/' "$ATH_MK"

# 3. Add sdio variant to ath config_package
sed -i 's/config_package,ath,regular smallbuffers/config_package,ath,regular sdio smallbuffers/' "$ATH_MK"

# 4. Tie WCN36XX's backports config to its kmod package
sed -i '/config-$(CONFIG_TARGET_ipq40xx) += ATH10K_AHB/a config-$(call config_package,wcn36xx) += WCN36XX' "$ATH_MK"

# 5. Add ath10k-sdio config line (after ath10k regular line)
sed -i '/config-.*config_package,ath10k,regular)/a config-$(call config_package,ath10k-sdio,sdio) += ATH10K ATH10K_SDIO' "$ATH_MK"

# 6. Extend kmod-ath DEPENDS for msm89xx
sed -i 's/@PCI_SUPPORT||USB_SUPPORT||TARGET_ath79 +kmod-mac80211/@PCI_SUPPORT||USB_SUPPORT||TARGET_ath79||TARGET_ath25||TARGET_msm89xx +kmod-mac80211/' "$ATH_MK"

# 7. Add ath10k-sdio to LEDS and THERMAL depends
sed -i 's/PACKAGE_kmod-ath10k || PACKAGE_kmod-ath10k-smallbuffers/PACKAGE_kmod-ath10k || PACKAGE_kmod-ath10k-sdio || PACKAGE_kmod-ath10k-smallbuffers/g' "$ATH_MK"

# 8. Add default y to ATH10K_THERMAL (insert before its depends line)
sed -i '/config ATH10K_THERMAL/{n;n;/depends/i\               default y
}' "$ATH_MK"

# 9. Append KernelPackage/ath10k-sdio and KernelPackage/wcn36xx definitions
cat >> "$ATH_MK" << 'EOF'

define KernelPackage/ath10k-sdio
  $(call KernelPackage/mac80211/Default)
  TITLE:=Atheros 802.11ac SDIO wireless cards support
  URL:=https://wireless.wiki.kernel.org/en/users/drivers/ath10k
  DEPENDS+= +kmod-ath +kmod-mmc +@DRIVER_11AC_SUPPORT \
	+ATH10K_THERMAL:kmod-hwmon-core +ATH10K_THERMAL:kmod-thermal
  FILES:= \
	$(PKG_BUILD_DIR)/drivers/net/wireless/ath/ath10k/ath10k_core.ko \
	$(PKG_BUILD_DIR)/drivers/net/wireless/ath/ath10k/ath10k_sdio.ko
  AUTOLOAD:=$(call AutoProbe,ath10k_core)
  MODPARAMS.ath10k_core:=frame_mode=2
  VARIANT:=sdio
endef

define KernelPackage/ath10k-sdio/description
This module adds support for wireless adapters based on
Atheros IEEE 802.11ac family of chipsets with SDIO bus.
endef

define KernelPackage/wcn36xx
  $(call KernelPackage/mac80211/Default)
  TITLE:=Qualcomm Atheros WCN3660/3680 support
  URL:=https://wireless.wiki.kernel.org/en/users/drivers/wcn36xx
  DEPENDS+= @TARGET_msm89xx +kmod-ath +kmod-qcom-rproc-wcnss +kmod-qcom-qmi-helpers
  FILES:=$(PKG_BUILD_DIR)/drivers/net/wireless/ath/wcn36xx/wcn36xx.ko
  AUTOLOAD:=$(call AutoProbe,wcn36xx)
endef

define KernelPackage/wcn36xx/description
This module adds support for Qualcomm Atheros WCN3660/3680 Wireless
blocks in some Qualcomm SoCs
endef
EOF

require_line 'ath10k ath10k-sdio ath10k-smallbuffers'
require_line 'wil6210 wcn36xx qcom-qmi-helpers'
require_line 'WCN36XX_DEBUGFS'
require_line 'config-$(call config_package,wcn36xx) += WCN36XX'
require_line 'config-$(call config_package,ath10k-sdio,sdio) += ATH10K ATH10K_SDIO'
require_line 'DEPENDS+= @PCI_SUPPORT||USB_SUPPORT||TARGET_ath79||TARGET_ath25||TARGET_msm89xx +kmod-mac80211'
require_line 'depends on PACKAGE_kmod-ath10k || PACKAGE_kmod-ath10k-sdio || PACKAGE_kmod-ath10k-smallbuffers'
require_line 'define KernelPackage/ath10k-sdio'
require_line 'define KernelPackage/wcn36xx'
require_line 'DEPENDS+= @TARGET_msm89xx +kmod-ath +kmod-qcom-rproc-wcnss +kmod-qcom-qmi-helpers'
if ! awk '
    /config ATH10K_THERMAL/ { in_thermal = 1; next }
    in_thermal && /config / { exit }
    in_thermal && /default y/ { has_default = 1 }
    END { exit !has_default }
' "$ATH_MK"; then
    echo "Error: failed to enable the ATH10K_THERMAL default" >&2
    exit 1
fi

printf '\n# MSM8916_OPENWRT_MAC80211_PATCHES\n' >> "$ATH_MK"
echo "mac80211 patches applied successfully"
