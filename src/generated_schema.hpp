#pragma once

// Keep IDE indexing and non-CMake parsing working by forwarding to the generated
// schema header from the active build tree. Real builds still regenerate these files.

#if defined(SEDSPRINTF_USE_OVERLAY_SCHEMA)
#  if __has_include("../build/generated_overlay/generated_schema.hpp")
#    include "../build/generated_overlay/generated_schema.hpp"
#  elif __has_include("generated_schema.hpp")
#    include_next "generated_schema.hpp"
#  else
#    error "generated overlay schema header not found"
#  endif
#else
#  if __has_include("../build/generated/generated_schema.hpp")
#    include "../build/generated/generated_schema.hpp"
#  elif __has_include("generated_schema.hpp")
#    include_next "generated_schema.hpp"
#  else
#    error "generated schema header not found"
#  endif
#endif
