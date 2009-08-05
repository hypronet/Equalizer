
/* Copyright (c) 2006-2009, Stefan Eilemann <eile@equalizergraphics.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef EQ_VERSION_H
#define EQ_VERSION_H

#include <eq/base/base.h>

#include <string>

namespace eq
{
    // Equalizer version macros and functions
#   define EQ_VERSION_MAJOR 0 //!< The current major version
#   define EQ_VERSION_MINOR 9
#   define EQ_VERSION_PATCH 0

    /** Information about the current Equalizer version. */
    class Version
    {
    public:
        /** @return the current major version of Equalizer */
        static uint32_t getMajor();
        static uint32_t getMinor();
        static uint32_t getPatch();

        static uint32_t getInt();
        static float    getFloat();
        static std::string getString();
    };
}

#endif //EQ_VERSION_H
