This is the new **Conquest DICOM Server** source code repository, it is the first ever release of the full source code of Conquest Dicom server with full source code, including the windows GUI.

Its USP is simplicity for starters, speed for serious users, and endless configurability for nerds and anybody wanting to automate DICOM workflows.

[Web page] http://www.natura-ingenium.nl/dicom.html

[Forum] (https://forum.image-systems.biz/forum/index.php?board/33-conquest-users/)

[Download current stable release 1.5.0f] (http://natura-ingenium.nl/dicomserver/dicomserver150f.zip)

The commit corresponding to 1.5.0f is here: https://github.com/marcelvanherk/Conquest-DICOM-Server/commit/3722dd648a905a9591cc31c016674ae3578a3ac4

**Conquest DICOM Server** is a group effort and I would like to thank Mark Oskin for the initial library; Lambert Zijp for years of development while at NKI, especially fixing bugs in the transport layers, and adding initial jpeg codecs; Lennert Ploeger for optimising the array implementation and the Delphi exe launcher; Bruce Barton for greatly extending compression and decompression, many many bug fixes and startup code; and Luiz Oliveira for enthousiastically helping developing WEBDicom, providing a sample webDICOM/Cornerstone based viewer, and endless bug finding.

Open source code used includes the Independent JPEG Group's software; charls by Jan de Vaan; François PIETTE's Delphi socket components, Lua5.1 by Roberto Ierusalimschy, Luasocket by Diego Nehab, OpenJpeg (for jepg2000) by Herve Drolon, FreeImage Team, and CS Systemes d'Information, France, SQlite by D Richard Hipp, Delphi UService from an unknown source, MiTeC DBF Table by Michal Mutl, Lunatic Python by Gustavo Niemeyer and PhP Router by Bramus Van Damme. 

In addition, the following minimised sample web viewers are included: papaya (https://github.com/rii-mango/Papaya, build 1446) and ohif (https://github.com/OHIF/Viewers, 1.03). 

The app/newweb code also contains unmaintained interfaces for dwv (https://github.com/ivmartel/dwv), and weasis (https://nroduit.github.io/en/).

---

## Addendum: DivNet-MakeIT fork — about this branch

This is the DivNet-MakeIT fork of [marcelvanherk/Conquest-DICOM-Server](https://github.com/marcelvanherk/Conquest-DICOM-Server); the text above is the upstream readme, unchanged.
Branch `divnet-stable` is upstream master `16cfc3e` (1.5.0f) plus one packaging commit, `651eee2`: `maklinux` copies `webserver/htdocs` with `cp -rn`, so files already present in the web root (for example a locally maintained web front end) are not overwritten on rebuild, and `linux/dgatesop.lst` lists the Storage Commitment Push Model SOP class (1.2.840.10008.1.20.1). No dgate source code is changed. Status: stable deployment branch.

Branches of this fork:

- `sc-outgoing` (default branch): variant C, storage commitment for incoming and outgoing data, without DICOM TLS (opt-in), on upstream `f8c0057` (1.5.0g4); documentation in [docs/README.md](https://github.com/DivNet-MakeIT/Conquest-DICOM-Server/blob/sc-outgoing/docs/README.md).
- `sc-outgoing-tls`: variant B, storage commitment for incoming and outgoing data plus DICOM TLS, including a legacy TLS profile (all opt-in), on upstream `f8c0057` (1.5.0g4); documentation in [docs/README.md](https://github.com/DivNet-MakeIT/Conquest-DICOM-Server/blob/sc-outgoing-tls/docs/README.md).
- `sc-incoming`: variant A, storage commitment SCP for incoming data only (opt-in): the same two commits as `storage-commitment`, refreshed on upstream `f8c0057` (1.5.0g4), with a Windows binary in a separate last commit.
- `storage-commitment`: the original two-commit version of the incoming storage commitment SCP, on upstream `41575e8` (1.5.0g3); kept unchanged, superseded by `sc-incoming`.
- `divnet-stable`: upstream `16cfc3e` (1.5.0f) plus one Linux packaging commit (web files installed without overwriting existing ones; Storage Commitment SOP class listed in `linux/dgatesop.lst`).
- `divnet-stable-1c2ff06`: the same packaging commit on upstream `1c2ff06` (1.5.0g).
- `master`: untouched copy of upstream master at `16cfc3e` (1.5.0f); not kept in sync with upstream.
