/***************************************************************************/
/*                                                                         */
/*  internal.h                                                             */
/*                                                                         */
/*    Internal header files (specification only).                          */
/*                                                                         */
/*  Copyright 1996-2018 by                                                 */
/*  David Turner, Robert Wilhelm, and Werner Lemberg.                      */
/*                                                                         */
/*  This file is part of the FreeType project, and may only be used,       */
/*  modified, and distributed under the terms of the FreeType project      */
/*  license, LICENSE.TXT.  By continuing to use, modify, or distribute     */
/*  this file you indicate that you have read the license and              */
/*  understand and accept it fully.                                        */
/*                                                                         */
/***************************************************************************/


  /*************************************************************************/
  /*                                                                       */
  /* This file is automatically included by `ft2build.h'.                  */
  /* Do not include it manually!                                           */
  /*                                                                       */
  /*************************************************************************/


#define FT_INTERNAL_OBJECTS_H             "ftobjs.h"
#define FT_INTERNAL_OBJECTS_H_FT          "freetype/ftobjs.h"
#define FT_INTERNAL_STREAM_H              "ftstream.h"
#define FT_INTERNAL_STREAM_H_FT           "freetype/ftstream.h"
#define FT_INTERNAL_MEMORY_H              "ftmemory.h"
#define FT_INTERNAL_MEMORY_H_FT           "freetype/ftmemory.h"
#define FT_INTERNAL_DEBUG_H               "ftdebug.h"
#define FT_INTERNAL_DEBUG_H_FT            "freetype/ftdebug.h"
#define FT_INTERNAL_CALC_H                "ftcalc.h"
#define FT_INTERNAL_CALC_H_FT             "freetype/ftcalc.h"
#define FT_INTERNAL_HASH_H                "fthash.h"
#define FT_INTERNAL_DRIVER_H              "ftdrv.h"
#define FT_INTERNAL_TRACE_H               "fttrace.h"
#define FT_INTERNAL_GLYPH_LOADER_H        "ftgloadr.h"
#define FT_INTERNAL_SFNT_H                "sfnt.h"
#define FT_INTERNAL_SERVICE_H             "ftserv.h"
#define FT_INTERNAL_SERVICE_H_FT          "freetype/ftserv.h"
#define FT_INTERNAL_RFORK_H               "ftrfork.h"
#define FT_INTERNAL_VALIDATE_H            "ftvalid.h"

#define FT_INTERNAL_TRUETYPE_TYPES_H      "tttypes.h"
#define FT_INTERNAL_TRUETYPE_TYPES_H_FT   "freetype/tttypes.h"
#define FT_INTERNAL_TYPE1_TYPES_H         "t1types.h"

#define FT_INTERNAL_POSTSCRIPT_AUX_H      "psaux.h"
#define FT_INTERNAL_POSTSCRIPT_HINTS_H    "pshints.h"
#define FT_INTERNAL_POSTSCRIPT_PROPS_H    "ftpsprop.h"

#define FT_INTERNAL_AUTOHINT_H            "autohint.h"

#define FT_INTERNAL_CFF_TYPES_H           "cfftypes.h"
#define FT_INTERNAL_CFF_OBJECTS_TYPES_H   "cffotypes.h"


#if defined( _MSC_VER )      /* Visual C++ (and Intel C++) */

  /* We disable the warning `conditional expression is constant' here */
  /* in order to compile cleanly with the maximum level of warnings.  */
  /* In particular, the warning complains about stuff like `while(0)' */
  /* which is very useful in macro definitions.  There is no benefit  */
  /* in having it enabled.                                            */
#pragma warning( disable : 4127 )

#endif /* _MSC_VER */


/* END */
