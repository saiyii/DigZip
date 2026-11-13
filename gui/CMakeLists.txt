find_package(Qt6 6.7 REQUIRED COMPONENTS Widgets LinguistTools)
qt_standard_project_setup(I18N_SOURCE_LANGUAGE en)

# Everything but main(), shared with the tests.
add_library(digzip_gui_lib STATIC
    entry_model.cpp
    job.cpp
    main_window.cpp
    support.cpp)
target_include_directories(digzip_gui_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(digzip_gui_lib PUBLIC DigZip::engine Qt6::Widgets)
digzip_set_warnings(digzip_gui_lib)

qt_add_resources(digzip_gui_lib icons PREFIX /icons BASE icons FILES icons/extract.svg)

# The English plural forms ("1 file", "2 files"), embedded under :/i18n.
qt_add_translations(digzip_gui_lib
    SOURCE_TARGETS digzip_gui_lib
    PLURALS_TS_FILE translations/digzip_en.ts)

qt_add_executable(digzip-gui WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(digzip-gui PRIVATE digzip_gui_lib)
set_target_properties(digzip-gui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME DigZip
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.saiyii.DigZip
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
if(APPLE)
    set_target_properties(digzip-gui PROPERTIES OUTPUT_NAME DigZip) # DigZip.app
endif()
digzip_set_warnings(digzip-gui)

# Directory of the Qt DLLs, for running the tests from the build tree.
set(DIGZIP_QT_BIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" PARENT_SCOPE)
