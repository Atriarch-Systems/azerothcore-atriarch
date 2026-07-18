# Ensure the module is correctly registered before linking
if(TARGET modules)
    # httplib (header-only) replaced libcurl; no extra link deps needed

    # Add local vendored nlohmann json (single-header at thirdparty/nlohmann/json.hpp)
    target_include_directories(modules PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/thirdparty
        ${CMAKE_CURRENT_LIST_DIR}/thirdparty/nlohmann
    )
endif()