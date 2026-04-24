set(srcs
            LCD/icna3312.c
            POWER/ds4730.c
            SPI/spi.c)

set(include_dirs
            LCD
            POWER
            SPI)

set(requires
            driver)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
