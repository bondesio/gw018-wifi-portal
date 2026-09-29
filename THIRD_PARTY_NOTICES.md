# Third-party notices

This repository is an overlay, not a redistribution of the complete GW018-DM
SDK. The build scripts fetch the SDK separately from
<https://github.com/jasperw1996/ambd_sdk_GW018-DM> and pin commit
`f144bf1b19dc869730ef09fc2515760245a26a97`. Credit for that GW018-DM SDK
adaptation belongs to its maintainer, `@jasperw1996`. Its README describes
adapting `@parasite85`'s <https://github.com/parasite85/rtl_firmware> for the
GW018-DM WBRG1/RTL8721CSM; that project is itself forked from
<https://github.com/hmsfeng/amb1_sdk>. The GW018-DM SDK repository is a fork
of Seeed Studio's AmebaD SDK:
<https://github.com/Seeed-Studio/seeed-ambd-sdk>.

This repository adds the button-operated Wi-Fi portal and build/release
overlay on top of that lineage. These credits do not imply endorsement or
affiliation. Consult each upstream repository for its own license and notices.

The SDK contains third-party Realtek/AmebaD code and binary materials with
their own notices and restrictions. Review the exact pinned project's files,
including its Realtek disclaimer and any notices packaged with the compiler
and ImageTool, before building for distribution or redistributing binaries.
The overlay license in `LICENSE` applies only to the original portal component
and this repository's scripts and documentation. It does not grant a license
to upstream source, SDK patches as applied to upstream-owned files, compiler,
ImageTool, or firmware dependencies.
