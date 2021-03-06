cmake_policy(SET CMP0015 NEW)

link_directories()
	
file(GLOB src
    "Include/*.h"
    "Source/*.cpp"
)
	
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_USE_GDIPLUS")

add_target(NAME RealSpace2 TYPE STATIC SOURCES "${src}")

target_include_directories(RealSpace2 PUBLIC
	.
	Include
	../CML/Include
	../MAI/Include
	../RealSpace/Include
	../sdk/dx9/include
	../sdk/rapidxml/include
	../sdk)

find_library(dxerr_path NAMES dxerr PATHS ../sdk/dx9/lib/x86)

target_link_libraries(RealSpace2 PUBLIC
	${dxerr_path})

find_library(BulletCollision_Debug_path NAMES BulletCollision PATHS ../sdk/bullet/lib/Debug)
find_library(LinearMath_Debug_path NAMES LinearMath PATHS ../sdk/bullet/lib/Debug)

target_link_libraries(RealSpace2 PUBLIC debug
	${BulletCollision_Debug_path}
	${LinearMath_Debug_path})

find_library(BulletCollision_Release_path NAMES BulletCollision PATHS ../sdk/bullet/lib/Release)
find_library(LinearMath_Release_path NAMES LinearMath PATHS ../sdk/bullet/lib/Release)

target_link_libraries(RealSpace2 PUBLIC optimized
	${BulletCollision_Release_path}
	${LinearMath_Release_path})