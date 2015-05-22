/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2015 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Kevin Dietrich
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "openvdb_writer.h"

OpenVDBWriter::OpenVDBWriter()
    : m_grids(new openvdb::GridPtrVec())
{
	m_meta_map.insertMeta("creator", openvdb::StringMetadata("Blender/OpenVDBWriter"));
}

OpenVDBWriter::~OpenVDBWriter()
{}

void OpenVDBWriter::insert(const openvdb::GridBase::Ptr &grid)
{
	m_grids->push_back(grid);
}

void OpenVDBWriter::insert(const openvdb::GridBase &grid)
{
	m_grids->push_back(grid.copyGrid());
}

void OpenVDBWriter::insertFloatMeta(const std::string &name, const float value)
{
	m_meta_map.insertMeta(name, openvdb::FloatMetadata(value));
}

void OpenVDBWriter::insertIntMeta(const std::string &name, const int value)
{
	m_meta_map.insertMeta(name, openvdb::Int32Metadata(value));
}

void OpenVDBWriter::insertVec3sMeta(const std::string &name, const openvdb::Vec3s value)
{
	m_meta_map.insertMeta(name, openvdb::Vec3SMetadata(value));
}

void OpenVDBWriter::insertVec3IMeta(const std::string &name, const openvdb::Vec3I value)
{
	m_meta_map.insertMeta(name, openvdb::Vec3IMetadata(value));
}

void OpenVDBWriter::insertMat4sMeta(const std::string &name, const float value[4][4])
{
	openvdb::Mat4s mat = openvdb::Mat4s(
			value[0][0], value[0][1], value[0][2], value[0][3],
	        value[1][0], value[1][1], value[1][2], value[1][3],
	        value[2][0], value[2][1], value[2][2], value[2][3],
	        value[3][0], value[3][1], value[3][2], value[3][3]);

	m_meta_map.insertMeta(name, openvdb::Mat4SMetadata(mat));
}

void OpenVDBWriter::setFileCompression(const int flags)
{
	m_flags = flags;
}

void OpenVDBWriter::write(const std::string &filename) const
{
	openvdb::io::File file(filename);
	file.setCompression(m_flags);
	file.write(*m_grids, m_meta_map);
	file.close();

	/* Should perhaps be an option at some point */
	m_grids->clear();
}
