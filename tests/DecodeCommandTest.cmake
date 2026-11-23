# Runs the decode command end to end: record a simulated transfer as raw frames, decode the recording, compare files.
# Expects -DVFDT=<vfdt executable> -DINPUT=<file to send> -DWORK=<scratch directory>.

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# Two full passes filmed at three captures per frame give the decoder several batches and an early finish.
execute_process(
    COMMAND "${VFDT}" simulate "${INPUT}" --screen 640x360 --cell 4 --camera 1000x600 --passes 2 --full
            --record "${WORK}/capture.rgb" --out "${WORK}/simulated"
    RESULT_VARIABLE simulateResult)
if(NOT simulateResult EQUAL 0)
    message(FATAL_ERROR "vfdt simulate failed with ${simulateResult}")
endif()

execute_process(
    COMMAND "${VFDT}" decode "${WORK}/capture.rgb" --raw 1000x600 --threads 3 --out "${WORK}/decoded"
    RESULT_VARIABLE decodeResult)
if(NOT decodeResult EQUAL 0)
    message(FATAL_ERROR "vfdt decode failed with ${decodeResult}")
endif()

get_filename_component(inputName "${INPUT}" NAME)
if(NOT EXISTS "${WORK}/decoded/${inputName}")
    message(FATAL_ERROR "vfdt decode did not write ${inputName}")
endif()
file(SHA256 "${INPUT}" sentHash)
file(SHA256 "${WORK}/decoded/${inputName}" decodedHash)
if(NOT sentHash STREQUAL decodedHash)
    message(FATAL_ERROR "decoded file differs from the one sent")
endif()
