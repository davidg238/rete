# Bake the manifest pin beside the build id (rete#243 D4/D5). Run at BUILD
# time like gen_build_id.cmake, and re-run every build, so the pin is the
# manifest actually current when this image was made. The heavy lifting —
# fetch from porta ($PORTA_MANIFEST_URL), verify the endpoint's hash claim,
# lint, loud repo fallback stamped source=repo — lives in
# protocol/tools/pin_manifest.py; this wrapper only fails the build when the
# pin cannot be made at all (a dirty manifest, a lying endpoint). An
# UNREACHABLE porta is not that: the script falls back to the repo file and
# says so, and the provision gate re-validates the pin later.
execute_process(
  COMMAND python3 ${SRC}/../../../protocol/tools/pin_manifest.py
          --out-dir ${BUILD}
          --emit-c ${OUT_C}
  RESULT_VARIABLE PIN_RC
  OUTPUT_VARIABLE PIN_OUT
  ERROR_VARIABLE PIN_ERR)
if(NOT PIN_RC EQUAL 0)
  message(FATAL_ERROR "manifest pin failed:\n${PIN_OUT}${PIN_ERR}")
endif()
if(PIN_ERR)
  message(WARNING "${PIN_ERR}")
endif()
