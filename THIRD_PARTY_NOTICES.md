# Third-party notices

This project is distributed under the MIT License (see `LICENSE`). The
following third-party software, assets and data keep their own terms.

## Zectrix NOTE4 reference components

`components/zectrix_epd` and `components/zectrix_board` are copied unchanged
from the ZECTRIX NOTE4 e-paper reference demo
(https://github.com/itopinion/zectrix-note4-epd-demo), © Zectrix Lab, MIT
License.

## Noto Sans CJK HK

The bitmap fonts in `main/fonts/*.bin` were rasterised by `tools/gen_font.py`
from Noto Sans CJK HK Bold, © Adobe, licensed under the SIL Open Font
License 1.1 (`licenses/NOTO_SANS_CJK_OFL.txt`). The bitmaps are a modified
version of the font and are distributed under the same licence.

## QR Code generator library

`components/hkbus_core/third_party/qrcodegen.{c,h}` © Project Nayuki, MIT
License (https://www.nayuki.io/page/qr-code-generator-library).

## esptool-js

The browser installer published to GitHub Pages includes Espressif's
esptool-js (https://github.com/espressif/esptool-js), Apache License 2.0. The
release workflow copies its `bundle.js` and licence into `vendor/`.

## Espressif ESP-IDF and esp_codec_dev

Build dependencies, not copied into this project; see their own licences.
`espressif/esp_codec_dev` is downloaded by the ESP-IDF Component Manager.

## Arrival-time and route data

At run time the device reads real-time arrival information from the public
APIs of Kowloon Motor Bus, Citybus, Green Minibus, New Lantao Bus and MTR Bus
published through DATA.GOV.HK (https://data.gov.hk/), subject to the DATA.GOV.HK
terms and conditions. The settings page loads the route and stop list compiled
by the hkbus.app project (https://github.com/hkbus/hk-bus-crawling). This
project is not affiliated with any operator, DATA.GOV.HK or hkbus.app. The
filtering rules for each operator follow the behaviour of hkbus.app's open
ETA library (https://github.com/hkbus/hk-bus-eta); no code from it is included.
