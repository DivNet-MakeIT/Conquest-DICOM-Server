This is the new **Conquest DICOM Server** source code repository, it is the first ever release of the full source code of Conquest Dicom server with full source code, including the windows GUI.

Its USP is simplicity for starters, speed for serious users, and endless configurability for nerds and anybody wanting to automate DICOM workflows.

[Web page] http://www.natura-ingenium.nl/dicom.html

[Forum] (https://forum.iq-image.com/forum/index.php?board/33-conquest-users/)

[Download current stable release 1.5.0g] (http://natura-ingenium.nl/dicomserver/dicomserver150g.zip)

The commit corresponding to 1.5.0g (prior to the readme) is here: https://github.com/marcelvanherk/Conquest-DICOM-Server/tree/777e72736f34ce93bfebc3c390dad0185434486b

**Conquest DICOM Server** is a group effort and I would like to thank Mark Oskin for the initial library; Lambert Zijp for years of development while at NKI, especially fixing bugs in the transport layers, and adding initial jpeg codecs; Lennert Ploeger for optimising the array implementation and the Delphi exe launcher; Bruce Barton for greatly extending compression and decompression, many many bug fixes and startup code; and Luiz Oliveira for enthousiastically helping developing WEBDicom, providing a sample webDICOM/Cornerstone based viewer, and endless bug finding. Marcin Makalowski (DivNet/MakeIT divinus) has helped written the IP matching code for access control in 1.5.0g, and has written the DICOMWeb streaming code in Ladle.

Open source code used includes the Independent JPEG Group's software; charls by Jan de Vaan; François PIETTE's Delphi socket components, Lua5.1 by Roberto Ierusalimschy, Luasocket by Diego Nehab, OpenJpeg (for jepg2000) by Herve Drolon, FreeImage Team, and CS Systemes d'Information, France, SQlite by D Richard Hipp, Delphi UService from an unknown source, MiTeC DBF Table by Michal Mutl, Lunatic Python by Gustavo Niemeyer and PhP Router by Bramus Van Damme. 

In addition, the following minimised sample web viewers are included: papaya (https://github.com/rii-mango/Papaya, build 1446) and ohif (https://github.com/OHIF/Viewers, 1.03). 

The app/newweb code also contains unmaintained interfaces for dwv (https://github.com/ivmartel/dwv), and weasis (https://nroduit.github.io/en/).

---

## Addendum: DivNet-MakeIT fork — about this branch

This is the DivNet-MakeIT fork of [marcelvanherk/Conquest-DICOM-Server](https://github.com/marcelvanherk/Conquest-DICOM-Server); the text above is the upstream readme, unchanged.
Branch `sc-incoming` (variant A) adds a storage commitment SCP for incoming data to dgate, on upstream master `f8c0057` (1.5.0g4): Storage Commitment Push Model requests (N-ACTION) are answered with an N-EVENT-REPORT after each instance is checked in the database and on disk; requests from calling AEs not in acrnema.map are refused; the server prints "Build variant: SC-in" at startup. It is off by default (`EnableStorageCommitment = 0` in dicom.ini), so with default settings dgate behaves as 1.5.0g4. Release tag: `sc-incoming-v1`. Documentation for both variants: [docs/README.md](https://github.com/DivNet-MakeIT/Conquest-DICOM-Server/blob/sc-outgoing-tls/docs/README.md) on `sc-outgoing-tls`.

Branches of this fork:

- `divnet-stable` (default branch): upstream `16cfc3e` (1.5.0f) plus one Linux packaging commit (web files installed without overwriting existing ones; Storage Commitment SOP class listed in `linux/dgatesop.lst`).
- `divnet-stable-1c2ff06`: the same packaging commit on upstream `1c2ff06` (1.5.0g).
- `master`: copy of upstream master at `16cfc3e` (1.5.0f); not kept in sync with upstream.
- `sc-incoming`: variant A, storage commitment SCP for incoming data (opt-in), on upstream `f8c0057` (1.5.0g4).
- `sc-outgoing-tls`: variant B, storage commitment for incoming and outgoing data, DICOM TLS and verify-before-delete (all opt-in), on upstream `f8c0057` (1.5.0g4); documentation in [docs/README.md](https://github.com/DivNet-MakeIT/Conquest-DICOM-Server/blob/sc-outgoing-tls/docs/README.md).
- `storage-commitment`: earlier version of the incoming storage commitment SCP, on upstream `41575e8`; superseded by `sc-incoming`.
- `add-license-file`: adds a LICENSE file; merged upstream through pull request #51, kept for reference.
