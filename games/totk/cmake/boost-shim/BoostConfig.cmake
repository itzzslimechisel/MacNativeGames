# Minimal header-only Boost package for dynarmic (it only needs boost/icl and boost/variant headers).
# TOTK_BOOST_INCLUDE_DIR is set by games/totk/CMakeLists.txt to the fetched Boost header tree.
if(NOT TARGET Boost::boost)
  add_library(Boost::boost INTERFACE IMPORTED GLOBAL)
  set_target_properties(Boost::boost PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${TOTK_BOOST_INCLUDE_DIR}")
  add_library(Boost::headers ALIAS Boost::boost)
endif()
set(Boost_FOUND TRUE)
set(Boost_INCLUDE_DIRS "${TOTK_BOOST_INCLUDE_DIR}")
