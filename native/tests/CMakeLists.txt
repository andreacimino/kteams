# Regression tests: one Qt Test executable per module, registered with ctest.
find_package(Qt6 REQUIRED COMPONENTS Test)

function(kteams_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE kteams_core Qt6::Test)
    target_compile_definitions(${name} PRIVATE KTEAMS_QML_DIR="${CMAKE_SOURCE_DIR}/qml" KTEAMS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME ${name} COMMAND ${name})
    # No display needed (CI runs headless); the content tests render HTML with QTextDocument.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

kteams_test(tst_content)
kteams_test(tst_chatparser)
kteams_test(tst_trouterframes)
kteams_test(tst_calljson)
kteams_test(tst_sdpdialect)
kteams_test(tst_caches)
kteams_test(tst_teamsclient)
kteams_test(tst_oauth)
kteams_test(tst_fileuploader)
kteams_test(tst_composer)
kteams_test(tst_translations)
kteams_test(tst_presence)
kteams_test(tst_callengine)
kteams_test(tst_mediadevices)
kteams_test(tst_settingspage)
kteams_test(tst_imageviewer)
target_compile_definitions(tst_translations PRIVATE KTEAMS_TRANSLATIONS_DIR="${CMAKE_SOURCE_DIR}/translations")

# Every QML file must at least parse (qmllint fails on syntax errors; its many
# unqualified-access warnings about the `backend` context object are tolerated).
find_program(QMLLINT_EXECUTABLE NAMES qmllint qmllint6 HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" /usr/lib/qt6/bin)
if(QMLLINT_EXECUTABLE)
    file(GLOB KTEAMS_QML_FILES ${CMAKE_SOURCE_DIR}/qml/*.qml)
    add_test(NAME qml_syntax COMMAND ${QMLLINT_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/qml ${KTEAMS_QML_FILES})
else()
    message(STATUS "qmllint not found: the qml_syntax test is not registered")
endif()
kteams_test(tst_backendlogic)
kteams_test(tst_models)
kteams_test(tst_statecache)
kteams_test(tst_demo)

# Coverage report of src/ from the tests above: cmake --build build --target coverage
if(KTEAMS_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)
    if(NOT GCOVR_EXECUTABLE)
        message(WARNING "gcovr not found: the coverage target is not available (pacman -S gcovr, or pip install gcovr)")
    else()
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
            COMMAND ${GCOVR_EXECUTABLE} --root ${CMAKE_SOURCE_DIR} --filter ${CMAKE_SOURCE_DIR}/src/
                    --print-summary --txt ${CMAKE_BINARY_DIR}/coverage/summary.txt
                    --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
                    --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
                    ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS tst_content tst_chatparser tst_trouterframes tst_calljson tst_sdpdialect tst_caches tst_teamsclient tst_oauth tst_fileuploader tst_composer tst_translations tst_presence tst_callengine tst_mediadevices tst_settingspage tst_imageviewer tst_backendlogic tst_models tst_statecache tst_demo
            COMMENT "Running the tests and writing coverage/index.html")
    endif()
endif()
