# Third-party notices

This repository is an overlay, not a redistribution of the complete GW018-DM
SDK. The build scripts fetch the SDK separately from
<https://github.com/jasperw1996/ambd_sdk_GW018-DM> and pin commit
`f144bf1b19dc869730ef09fc2515760245a26a97`. That project identifies its
lineage as the Seeed Studio AmebaD SDK:
<https://github.com/Seeed-Studio/seeed-ambd-sdk>.

The SDK contains third-party Realtek/AmebaD code and binary materials with
their own notices and restrictions. Review the exact pinned project's files,
including its Realtek disclaimer and any notices packaged with the compiler
and ImageTool, before building for distribution or redistributing binaries.
The overlay license in `LICENSE` applies only to the original portal component
and this repository's scripts and documentation. It does not grant a license
to upstream source, SDK patches as applied to upstream-owned files, compiler,
ImageTool, or firmware dependencies.
