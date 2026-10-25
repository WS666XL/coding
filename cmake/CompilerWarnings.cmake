# 统一的编译告警配置。只对项目自身代码生效，第三方头通过 SYSTEM include 隔离。

set(LOWLAT_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wformat=2
    -Wundef
)

function(lowlat_target_warnings target)
    target_compile_options(${target} PRIVATE ${LOWLAT_WARNINGS})
endfunction()
