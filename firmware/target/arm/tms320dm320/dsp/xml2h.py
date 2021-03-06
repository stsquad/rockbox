#!/usr/bin/python
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#
# Copyright (C) 2007 Catalin Patulea <cat@vv.carleton.ca>
#
#  All files in this archive are subject to the GNU General Public License.
#  See the file COPYING in the source tree root for full license agreement.
# 
#  This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
#  KIND, either express or implied.
#

import sys, os.path, array, re
from xml.dom import Node
from xml.dom.minidom import parse


C_IDENT_RE = re.compile('^[0-9a-zA-Z_]+$')


def getText(nodelist):
    rc = ""
    for node in nodelist:
        if node.nodeType == node.TEXT_NODE:
            rc = rc + node.data
    return rc


def descendAll(root, tagname):
    for child in root.childNodes:
        if child.nodeType == Node.ELEMENT_NODE and child.tagName == tagname:
            yield child


def descend(root, tagname):
    return descendAll(root, tagname).next()


def getTagText(root, tagname):
    try:
        tag = descend(root, tagname)
    except StopIteration:
        return None
    return getText(tag.childNodes)


def main():
    dom = parse(sys.stdin)
    
    ofd = descend(dom, "ofd")
    object_file = descend(ofd, "object_file")
    object_file_name = descend(object_file, "name")
    
    out_filepath = getText(object_file_name.childNodes)
    sys.stderr.write("*.out filename (input): %s\n" % out_filepath)
    
    out_file = open(out_filepath, "rb")
    h_file = sys.stdout
    
    h_file.write("""#ifndef DSP_IMAGE
#define DSP_IMAGE
/*
 * Automatically generated by xml2h.py from %s.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 */
""" % out_filepath)

    # Section data and directory.
    h_directory = ["""
static const struct dsp_section dsp_image[] = {"""]
    
    ti_coff = descend(object_file, "ti_coff")
    for section in descendAll(ti_coff, "section"):
        page = int(getTagText(section, "page") or "0", 16)
        name = getTagText(section, "name")
        physical_addr = int(getTagText(section, "physical_addr"), 16)
        raw_data_size = int(getTagText(section, "raw_data_size"), 16)
        copy = getTagText(section, "copy")
        data = getTagText(section, "data")
        regular = getTagText(section, "regular")
        text = getTagText(section, "text")
        bss = getTagText(section, "bss")
        
        file_offsets = descend(section, "file_offsets")
        raw_data_ptr = int(getTagText(file_offsets, "raw_data_ptr"), 16)
        
        if copy:
            # Empirically, .debug* sections have this attribute set.
            sys.stderr.write(
                "%s: didn't copy debug section ('copy' attribute set)\n" %
                name)
            continue
        
        if raw_data_size == 0:
            sys.stderr.write("%s: not copying empty section\n" % name)
            continue

        if raw_data_size % 2 != 0:
            sys.stderr.write("%s: error, raw_data_size 0x%04x not a multiple "
                "of word size (2 bytes)\n" % (name, raw_data_size))
            break
        
        if data or regular or text:
            sys.stderr.write("%s: placing 0x%04x words at 0x%04x from offset "
                "0x%08x\n" % (
                name, raw_data_size >> 1, physical_addr, raw_data_ptr))

            sanitized_name = name.replace(".", "_")
            h_file.write(("static const unsigned short _section%s[] = {\n" %
                sanitized_name))

            out_file.seek(raw_data_ptr)
            data = array.array('H')
            data.fromfile(out_file, raw_data_size >> 1)
            h_file.write("\t")
            for word in data:
                h_file.write("0x%04x, " % word)
            h_file.write("""
};
""")
            
            h_directory.append("\t{_section%s, 0x%04x, 0x%04x}," % (
                sanitized_name, physical_addr, raw_data_size >> 1))
                
            continue
        
        if bss:
            sys.stderr.write("%s: bss section, 0x%04x words at 0x%04x\n" % (
                name, raw_data_size >> 1, physical_addr))
            
            h_directory.append("\t{NULL /* %s */, 0x%04x, 0x%04x}," % (
                name, physical_addr, raw_data_size >> 1))
            continue
        
        sys.stderr.write("%s: error, unprocessed section\n" % name)
    
    h_file.write("\n")

    h_directory.append("\t{NULL, 0, 0}")
    h_directory.append("};")
    
    h_file.write("\n".join(h_directory))
    h_file.write("\n")
    
    # Symbols.
    symbol_table = descend(ti_coff, "symbol_table")
    h_file.write("""
/* Symbol table, usable with the DSP_() macro (see dsp-target.h). */
""")
    for symbol in descendAll(symbol_table, "symbol"):
        name = getTagText(symbol, "name")
        kind = getTagText(symbol, "kind")
        value = int(getTagText(symbol, "value"), 16)
        
        if kind != "defined":
            continue
        
        if not C_IDENT_RE.match(name):
            continue
        
        h_file.write("#define %s 0x%04x\n" % (name, value))
    
    h_file.write("\n#endif\n")
    h_file.close()
    out_file.close()
    
    dom.unlink()


if __name__ == "__main__":
    main()
    
