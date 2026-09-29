# GW018-DM Wi-Fi provisioning firmware add-on

This bundle applies the button-operated Wi-Fi setup portal to the WBRG1
(RTL8721CSM) side of the Tuya GW018-DM and builds the firmware. It does not
include the vendor SDK or a prebuilt firmware image. The SDK is obtained from
the [Jasper GW018-DM SDK fork](https://github.com/jasperw1996/ambd_sdk_GW018-DM)
at the pinned commit in `tools/prepare-sdk.sh`; that fork is based on the
[Seeed AmebaD SDK](https://github.com/Seeed-Studio/seeed-ambd-sdk). Read the
upstream notices and terms before using or redistributing SDK-derived builds.

## Build host and requirements

The tested build path is Linux (Debian/Ubuntu, x86-64). Install the host tools:

```sh
sudo apt update
sudo apt install build-essential git curl tar python3 libc6-i386
```

`libc6-i386` is needed because the available vendor ARM compiler is a 32-bit
Linux executable. The build helper downloads the roughly 250 MB toolchain
archive from Seeed over HTTPS and verifies its SHA-256 before extraction. The
first build needs internet access. The archive itself is not committed here.
Reserve at least 10 GB of free disk space for the SDK checkout, extracted
toolchain, and build intermediates. macOS and Windows are not covered by this
helper.

## Get source, configure, and build

Choose an OTA server hostname or IPv4 address reachable by the gateway. The
gateway's existing OTA mechanism fetches `OTA_All.bin` over TCP port 8080; this
host must serve that filename at the endpoint expected by the SDK's OTA client.
The host value is ordinary configuration, not a Wi-Fi credential.

```sh
git clone https://github.com/bondesio/gw018-wifi-portal.git
cd gw018-wifi-portal
tools/prepare-sdk.sh ../gw018-sdk
python3 tools/apply-overlay.py ../gw018-sdk --ota-host <your-ota-hostname-or-ip>
tools/build.sh ../gw018-sdk
```

Replace the angle-bracketed example with your own reachable update-server host.
The scripts refuse to overwrite an existing SDK destination, require the exact
pinned revision and a clean checkout, and apply the reviewed integration patch
only after checking it. The build produces KM0 and KM4 firmware images and an
OTA package. `build.sh` prints each output's SHA-256. Expected outputs are:

- `project/realtek_amebaD_va0_example/GCC-RELEASE/project_lp/asdk/image/km0_boot_all.bin`
- `project/realtek_amebaD_va0_example/GCC-RELEASE/project_hp/asdk/image/km4_boot_all.bin`
- `project/realtek_amebaD_va0_example/GCC-RELEASE/project_hp/asdk/image/km0_km4_image2.bin`
- `project/realtek_amebaD_va0_example/GCC-RELEASE/project_hp/asdk/image/OTA_All.bin`

The OTA image is for the WBRG1 firmware. Do not use this procedure to update
the separate Zigbee (ZS3L) processor.

## Flashing

For first installation or recovery, use a 3.3 V USB-to-TTL UART adapter and
the Realtek/AmebaD ImageTool for Linux. The ImageTool is not bundled; obtain
`upload_image_tool_linux` and its supporting files from the
[official AmebaD Arduino tool package](https://github.com/Ameba-AIoT/ameba-arduino-d/tree/master/Arduino_package/ameba_d_tools_linux)
and review its instructions and notices.
The binary images alone are not enough to flash over UART. Stage these three
build outputs in the ImageTool's working directory, alongside its supplied
`imgtool_flashloader_amebad.bin` support file:

```text
project/realtek_amebaD_va0_example/GCC-RELEASE/project_lp/asdk/image/km0_boot_all.bin
project/realtek_amebaD_va0_example/GCC-RELEASE/project_hp/asdk/image/km4_boot_all.bin
project/realtek_amebaD_va0_example/GCC-RELEASE/project_hp/asdk/image/km0_km4_image2.bin
```

Connect ground to ground, gateway TX to adapter RX, and gateway RX to adapter
TX. Use the board's documented UART flashing header (P1). Do not connect the
adapter's VCC pin: power the gateway normally over USB-C. Confirm the board
pinout and voltage before connecting anything; an incorrect connection can
damage the board.

Power the gateway normally first, then connect the USB-UART adapter so the
gateway enters UART download mode. From the ImageTool directory with the three
images beside it, the upstream fork documents these Linux commands for the
RTL8721CSM target and `/dev/ttyUSB0` (replace the serial device path as needed):

```sh
./upload_image_tool_linux "$PWD" /dev/ttyUSB0 ameba_rtl8721csm Enable Enable 921600
./upload_image_tool_linux "$PWD" /dev/ttyUSB0 ameba_rtl8721csm Enable Disable 921600
```

The first command prepares/erases the target; the second writes the three
images in the current directory. Keep a known-good backup and do not interrupt
power during flashing. UART recovery may be needed if an incorrect or
incomplete image is written.

After a working custom firmware is installed, later updates can use its OTA
path: place the newly built `OTA_All.bin` on the configured update server at
the endpoint expected on TCP port 8080, then hold the gateway button for at
least three seconds while the gateway is on Wi-Fi. OTA requires the configured
host to be reachable and is not a substitute for UART recovery.

## Portal behavior and security

- A short button press opens or closes the `GW018-Setup` access point.
- Connect to it and browse to `http://192.168.43.1/` if the phone does not
  open the setup page automatically.
- Scan for a network or type its SSID, then submit the password. Credentials
  are provided at runtime and are not compiled into this repository.
- The setup page uses plain HTTP on the local setup network. Use it only while
  physically present; another nearby client could observe the submitted Wi-Fi
  credentials.
- The device saves the new profile only after association and DHCP succeed;
  on failure it retains the previous profile.
- A long button hold (at least three seconds) starts the base OTA action.

The portal's authored source and tooling are MIT-licensed as scoped in
`LICENSE`. The integration patch modifies files from the pinned upstream SDK;
it is provided as a change set for that project and does not relicense those
files or the SDK. See `THIRD_PARTY_NOTICES.md` and all upstream notices for
attribution and terms. This is not a standalone firmware image.
