#
# Libmagic develop files search.
#
# 
#

find_library(MAGIC_LIBRARY NAMES magic)
find_path(MAGIC_INCLUDE_DIR magic.h PATH_SUFFIXES include/magic include/file include ) # Find header
find_package_handle_standard_args(libmagic DEFAULT_MSG MAGIC_LIBRARY MAGIC_INCLUDE_DIR)

