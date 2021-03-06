/*
 *  Albion Remake
 *  Copyright (C) 2007 - 2015 Florian Ziesche
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "map/3d/map3d.hpp"
#include "map/3d/npc3d.hpp"
#include "object_light.hpp"

static const float tile_size = std_tile_size;

/*! map3d constructor
 */
map3d::map3d(labdata* lab_data_, array<lazy_xld, 3>& maps) :
lab_data(lab_data_), map_xlds(maps), clock_cb(bind(&map3d::clock_tick, this, placeholders::_1)) {
	map_loaded = false;
	cur_map_num = (~0u);
	mnpcs = nullptr;
	npcs_model = nullptr;
	npc_object_count = 0;

	ow_tiles = nullptr;
	floor_tiles = nullptr;
	ceiling_tiles = nullptr;
	
	wall_vertices = nullptr;
	wall_tex_coords = nullptr;
	wall_indices = nullptr;
	wall_model = nullptr;
	wall_sides = nullptr;

	fc_vertices = nullptr;
	fc_tex_coords = nullptr;
	fc_indices = nullptr;
	fc_tiles_model = nullptr;
	
	obj_vertices = nullptr;
	obj_ws_positions = nullptr;
	obj_tex_coords = nullptr;
	obj_indices = nullptr;
	objects_model = nullptr;

	last_tile_animation = SDL_GetTicks();
	object_light_ani_time = SDL_GetTicks();

	bg3d = new background3d();
	bg_loaded = false;

	// player
	player_light = new object_light<object_light_type::TORCH>(float3(0.0f, 20.0f, 0.0f));
	
	// sun
	sun_light = new light(0.0f, 100.0f, 0.0f);
	sun_light->set_type(light::LIGHT_TYPE::DIRECTIONAL);
	sce->add_light(sun_light);
	sun_distance = 100.0f;
	SDL_Surface* sun_light_tex = IMG_Load(floor::data_path("sun_light.png").c_str());
	sun_color_table.reserve((size_t)sun_light_tex->w);
	const int bpp = (int)sun_light_tex->format->BytesPerPixel;
	for(int i = 0; i < sun_light_tex->w; i++) {
		unsigned char* color = ((unsigned char*)sun_light_tex->pixels) + i*bpp;
		const float r = *(color + (sun_light_tex->format->Rshift / 8));
		const float g = *(color + (sun_light_tex->format->Gshift / 8));
		const float b = *(color + (sun_light_tex->format->Bshift / 8));
		sun_color_table.push_back(float3(r, g, b) / 255.0f);
	}
	SDL_FreeSurface(sun_light_tex);
	active_sun_light = false;
	sun_light->set_enabled(active_sun_light);
	
	//
	clck->add_tick_callback(ar_clock::CCBT_TICK, clock_cb);
}

/*! map3d destructor
 */
map3d::~map3d() {
	clck->delete_tick_callback(clock_cb);
	
	delete player_light;
	delete sun_light;
	delete bg3d;
	unload();
}

void map3d::load(const size_t& map_num) {
	if(cur_map_num == map_num) return;
	log_debug("loading map %u ...", map_num);
	unload();
	
	//
	if(map_num >= MAX_MAP_NUMBER) {
		log_error("invalid map number: %u!", map_num);
		return;
	}

	const auto map_data_size = map_xlds[map_num/100].get_object_size(map_num % 100);
	if(map_data_size == 0) {
		log_error("map %u is empty!", map_num);
		return;
	}
	
	auto object_data = map_xlds[map_num/100].get_object_data(map_num % 100);
	const auto cur_map_data = object_data->data();
	
	// get map infos
	// header
	const size_t header_len = 10;
	size_t npc_data_len = (size_t)cur_map_data[1];
	map_size.set(cur_map_data[4], cur_map_data[5]);
	cur_labdata_num = cur_map_data[6];
	map_palette = (size_t)cur_map_data[8];
	
	cout << 0 << ": " << (size_t)cur_map_data[0] << endl;
	cout << 3 << ": " << (size_t)cur_map_data[3] << endl;
	cout << 7 << ": " << (size_t)cur_map_data[7] << endl;
	cout << 9 << ": " << (size_t)cur_map_data[9] << endl;
	cout << "map_palette: " << map_palette << endl;

	for(size_t i = 0; i < 32; i++) {
		cout << ((size_t)cur_map_data[i] < 16 ? "0":"") << hex << (size_t)cur_map_data[i] << dec;
	}
	cout << endl;
	
	// npc/monster info
	if(npc_data_len == 0x00) npc_data_len = 32 * 10;
	else if(npc_data_len == 0x40) npc_data_len = 96 * 10;
	else npc_data_len *= 10;
	
	// actual map data
	ow_tiles = new unsigned int[map_size.x*map_size.y];
	floor_tiles = new unsigned int[map_size.x*map_size.y];
	ceiling_tiles = new unsigned int[map_size.x*map_size.y];
	
	lab_data->load((cur_labdata_num < 100 ? cur_labdata_num-1 : cur_labdata_num), map_palette-1);
	
	if(lab_data->get_lab_info()->background != 0) {
		bg3d->load(lab_data->get_lab_info()->background-1, map_palette-1);
		active_sun_light = true;
	}
	else {
		bg3d->unload();
		active_sun_light = false;
	}
	bg_loaded = true;
	sun_light->set_enabled(active_sun_light);

	//
	/*
		7 = y streckung (*2.25), kein einfluss auf objekte?
		8 = normale tile/textur gr��e ### y * 1.87, kein einfluss auf objekte?
		9 = normale wall textur gr��e, senkung der decke, einfluss auf objekte?
		A = x streckung (*2.25), kein einfluss auf objekte?
	*/
	const float floor_height = 0.0f;
	float ceiling_height = std_tile_size;
	/*switch(lab_data->get_lab_info()->scale_2) {
		case 0x7:
			ceiling_height *= 2.0f;
			break;
		case 0x8:
			break;
		case 0x9:
			ceiling_height *= 0.5f;
			break;
		case 0xA:
			ceiling_height *= 0.5f;
			break;
		default:
			break;
	}*/

	// map scaling ...
	float2 map_scale(1.0f, 1.0f);
	
	// scale byte 1
	/*if(lab_data->get_lab_info()->scale_1 == 0x1) {
		// everything is fine, nothing to see here, move on ...
	}
	else if(lab_data->get_lab_info()->scale_1 == 0x2) {
		map_scale.y = float(lab_data->get_wall(101)->x_size) / float(lab_data->get_wall(101)->y_size);
		
		map_scale.x *= 1.2f;
		map_scale.y *= 1.2f;
	}
	else if(lab_data->get_lab_info()->scale_1 == 0x3) {
		map_scale.y = float(lab_data->get_wall(101)->x_size) / float(lab_data->get_wall(101)->y_size);
		
		map_scale.y *= 1.5f;
	}*/

	// scale byte 2
	/*if(lab_data->get_lab_info()->scale_2 == 0x7) map_scale.y = 2.25f;
	//if(lab_data->get_lab_info()->scale_2 == 0x8) map_scale.y = 1.87f;
	if(lab_data->get_lab_info()->scale_2 == 0x8) map_scale.y = 1.2f;
	if(lab_data->get_lab_info()->scale_2 == 0xA) map_scale.x = 2.25f;*/
	
	// TODO: apply map scale and new size to all other classes (player3d, collision detection!)

	//tile_size = std_tile_size * map_scale.x;
	ceiling_height = map_scale.y * float(lab_data->get_wall(101)->y_size)/8.0f;

	// get tiles
	const size_t map_data_offset = npc_data_len + header_len;
	const size_t map_data_len = (map_size.x * map_size.y) * 3;
	for(size_t i = map_data_offset, tile_num = 0; i < (map_data_offset + map_data_len); i+=3, tile_num++) {
		ow_tiles[tile_num] = (unsigned int)cur_map_data[i];
		floor_tiles[tile_num] = (unsigned int)cur_map_data[i+1];
		ceiling_tiles[tile_num] = (unsigned int)cur_map_data[i+2];
	}
	
	// events
	const size_t events_offset = map_data_offset + map_data_len;
	mevents.load(cur_map_data, map_data_size, events_offset, map_size);

	// load npc/monster data (has to be done here, b/c we need info that is available only after all events have been loaded)
	mnpcs = new map_npcs();
	mnpcs->load(cur_map_data, mevents.get_end_offset());
	const vector<map_npcs::map_npc*>& npc_data = mnpcs->get_npcs();
	for(vector<map_npcs::map_npc*>::const_iterator npc_iter = npc_data.begin(); npc_iter != npc_data.end(); npc_iter++) {
		npcs.push_back(new npc3d(this));
		npcs.back()->set_npc_data(*npc_iter);
	}
	
	// create npc object models
	unsigned int npc_count = (unsigned int)npcs.size();
	npc_object_count = 0;
	for(unsigned int i = 0; i < npc_count; i++) {
		const labdata::lab_object* obj = lab_data->get_object(npc_data[i]->object_num);
		if(obj == nullptr) continue;
		
		npc_object_count += obj->sub_object_count;
	}
	
	npcs_vertices = new float3[npc_object_count*4];
	npcs_ws_positions = new float3[npc_object_count*4];
	npcs_tex_coords = new float2[npc_object_count*4];
	npcs_indices = new uint3*[1];
	npcs_indices[0] = new uint3[npc_object_count*2];
	unsigned int* npcs_index_count = new unsigned int[1];
	npcs_index_count[0] = npc_object_count*2;
	
	unsigned int npc_index = 0, npc_index_num = 0;
	for(unsigned int i = 0; i < npc_count; i++) {
		const labdata::lab_object* obj = lab_data->get_object(npc_data[i]->object_num);
		if(obj == nullptr) continue;
		
		//
		for(size_t so = 0; so < obj->sub_object_count; so++) {
			const labdata::lab_object_info* sub_object = obj->sub_objects[so];
			//const float2 obj_size = float2(sub_object->x_size, sub_object->y_size);
			//const float2 obj_scale = float2(sub_object->x_scale, sub_object->y_scale);
			const float2& tc_b = sub_object->tex_coord_begin[0];
			const float2& tc_e = sub_object->tex_coord_end[0];
			const float x_size = float(sub_object->x_scale)/32.0f;
			const float y_size = float(sub_object->y_scale)/32.0f;
			float3 offset = float3(obj->offset[so].x, -obj->offset[so].y, obj->offset[so].z)/32.0f;
			
			if(sub_object->type & 0x4) {
				if(obj->offset[so].z == 0) {
					npcs_vertices[npc_index + 0].set((-x_size/2.0f), 0.001f, (-y_size/2.0f));
					npcs_vertices[npc_index + 1].set((-x_size/2.0f), 0.001f, (y_size/2.0f));
					npcs_vertices[npc_index + 2].set((x_size/2.0f), 0.001f, (-y_size/2.0f));
					npcs_vertices[npc_index + 3].set((x_size/2.0f), 0.001f, (y_size/2.0f));
				}
				else {
					npcs_vertices[npc_index + 0].set((x_size/2.0f), 0.0f, (-y_size/2.0f));
					npcs_vertices[npc_index + 1].set((x_size/2.0f), 0.0f, (y_size/2.0f));
					npcs_vertices[npc_index + 2].set((-x_size/2.0f), 0.0f, (-y_size/2.0f));
					npcs_vertices[npc_index + 3].set((-x_size/2.0f), 0.0f, (y_size/2.0f));
				}
			}
			else {
				npcs_vertices[npc_index + 0].set((-x_size/2.0f), (y_size/2.0f), 0.0f);
				npcs_vertices[npc_index + 1].set((-x_size/2.0f), (-y_size/2.0f), 0.0f);
				npcs_vertices[npc_index + 2].set((x_size/2.0f), (y_size/2.0f), 0.0f);
				npcs_vertices[npc_index + 3].set((x_size/2.0f), (-y_size/2.0f), 0.0f);
				offset.z += (y_size/2.0f);
			}
			
			const float2 npc_pos = float2(*npc_data[i]->position);
			npcs_ws_positions[npc_index + 0] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			npcs_ws_positions[npc_index + 1] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			npcs_ws_positions[npc_index + 2] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			npcs_ws_positions[npc_index + 3] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			
			npcs_tex_coords[npc_index + 0].set(tc_e.x, tc_b.y);
			npcs_tex_coords[npc_index + 1].set(tc_e.x, tc_e.y);
			npcs_tex_coords[npc_index + 2].set(tc_b.x, tc_b.y);
			npcs_tex_coords[npc_index + 3].set(tc_b.x, tc_e.y);
			
			npcs_indices[0][npc_index_num*2].set(npc_index + 1, npc_index + 0, npc_index + 2);
			npcs_indices[0][npc_index_num*2 + 1].set(npc_index + 2, npc_index + 3, npc_index + 1);
			
			npc_index_num++;
			npc_index+=4;
		}
	}
	
	npcs_model = new map_objects();
	npcs_model->load_from_memory(1, npc_object_count*4, npcs_vertices, npcs_tex_coords, npcs_index_count, npcs_indices);
	npcs_model->set_ws_positions(npcs_ws_positions);
	npcs_model->set_material(lab_data->get_object_material());
	
	// compute which wall sides we need
	wall_sides = new unsigned char[map_size.x*map_size.y];
	for(size_t y = 0; y < map_size.y; y++) {
		for(size_t x = 0; x < map_size.x; x++) {
			if(ow_tiles[y*map_size.x + x] >= 101) {
				unsigned char& side = wall_sides[y*map_size.x + x];
				const labdata::lab_wall* wall = lab_data->get_wall(ow_tiles[y*map_size.x + x]);
				if(wall == nullptr) continue;
				
				const labdata::lab_wall* walls[] = {
					(y > 0 ? lab_data->get_wall(ow_tiles[(y-1)*map_size.x + x]) : nullptr),				// north
					(x != map_size.x-1 ? lab_data->get_wall(ow_tiles[y*map_size.x + x+1]) : nullptr),		// east
					(y != map_size.y-1 ? lab_data->get_wall(ow_tiles[(y+1)*map_size.x + x]) : nullptr),	// south
					(x > 0 ? lab_data->get_wall(ow_tiles[y*map_size.x + x-1]) : nullptr),					// west
				};
				
				side = 0;
				for(size_t i = 0; i < 4; i++) {
					// TODO: does >=0x20 apply to all kinds of transparency?
					if(walls[i] == nullptr ||
					   (wall->type < 0x20 && walls[i]->type >= 0x20)) {
						side |= (unsigned char)(1 << i);
					}
				}
			}
		}
	}
	
	// create map models
	unsigned int floor_count = 0, ceiling_count = 0, wall_count = 0, wall_cutalpha_count = 0, wall_transp_obj_count = 0, wall_transp_count = 0, object_count = 0, sub_object_count = 0;
	wall_ani_count = 0;
	fc_ani_count = 0;
	obj_ani_count = 0;
	for(size_t y = 0; y < map_size.y; y++) {
		for(size_t x = 0; x < map_size.x; x++) {
			if(floor_tiles[y*map_size.x + x] > 0) {
				const labdata::lab_floor* floor = lab_data->get_floor(floor_tiles[y*map_size.x + x]);
				if(floor->animation > 1) fc_ani_count++;

				floor_count++;
			}
			if(ceiling_tiles[y*map_size.x + x] > 0) {
				const labdata::lab_floor* ceiling = lab_data->get_floor(ceiling_tiles[y*map_size.x + x]);
				if(ceiling->animation > 1) fc_ani_count++;

				ceiling_count++;
			}
			if(ow_tiles[y*map_size.x + x] >= 101) {
				const labdata::lab_wall* wall = lab_data->get_wall(ow_tiles[y*map_size.x + x]);
				if(wall == nullptr) continue;
				if(wall->type & labdata::WT_TRANSPARENT) wall_transp_obj_count++;
								
				unsigned char& side = wall_sides[y*map_size.x + x];
				for(size_t i = 0; i < 4; i++) {
					if((side & (unsigned char)(1 << i)) > 0) {
						if(wall->animation > 1) wall_ani_count++;
						
						if(wall->type & labdata::WT_TRANSPARENT) {
							wall_transp_count++;
							wall_count++;
							if(wall->animation > 1) wall_ani_count++;
						}
						else if(wall->type & labdata::WT_CUT_ALPHA) {
							wall_cutalpha_count++;
							wall_count++;
							if(wall->animation > 1) wall_ani_count++;
						}
						
						wall_count++;
					}
				}
			}
			if(ow_tiles[y*map_size.x + x] > 0 && ow_tiles[y*map_size.x + x] < 101) {
				const labdata::lab_object* obj = lab_data->get_object(ow_tiles[y*map_size.x + x]);
				if(obj != nullptr) {
					if(obj->animated) obj_ani_count += obj->sub_object_count;

					sub_object_count += obj->sub_object_count;
					object_count++;
				}
			}
		}
	}
	wall_transp_obj_count *= 2; // transparent walls are double-sided
	
	unsigned int fc_count = floor_count + ceiling_count;

	// map model
	wall_vertices = new float3[wall_count*4];
	wall_tex_coords = new float2[wall_count*4];
	wall_indices = new uint3*[1+wall_transp_obj_count];
	unsigned int* wall_index_count = new unsigned int[1+wall_transp_obj_count];
	wall_indices[0] = new uint3[(wall_count - wall_transp_count*2)*2];
	wall_index_count[0] = (unsigned int)(wall_count - wall_transp_count*2)*2;

	// floors/ceilings model
	fc_vertices = new float3[fc_count*4];
	fc_tex_coords = new float2[fc_count*4];
	fc_indices = new uint3*[1];
	fc_indices[0] = new uint3[fc_count*2];
	unsigned int* fc_index_count = new unsigned int[1];
	fc_index_count[0] = (unsigned int)fc_count*2;
	
	// map objects
	obj_vertices = new float3[sub_object_count*4];
	obj_ws_positions = new float3[sub_object_count*4];
	obj_tex_coords = new float2[sub_object_count*4];
	obj_indices = new uint3*[1];
	obj_indices[0] = new uint3[sub_object_count*2];
	unsigned int* obj_index_count = new unsigned int[1];
	obj_index_count[0] = (unsigned int)sub_object_count*2;
	
	unsigned int fc_num = 0, wall_num = 0, object_num = 0;

	// set animation offset (this will be used later for updating the texture coordinates)
	fc_ani_offset = (fc_count - fc_ani_count)*4;
	wall_ani_offset = (wall_count - wall_ani_count)*4;
	obj_ani_offset = (sub_object_count - obj_ani_count)*4;
	unsigned int fc_ani_num = 0, wall_ani_num = 0, obj_ani_num = 0;
	unsigned int fc_static_num = 0, wall_static_num = 0, obj_static_num = 0;
	unsigned int wall_subobj = 0;
	
	for(unsigned int y = 0; y < (unsigned int)map_size.y; y++) {
		for(unsigned int x = 0; x < (unsigned int)map_size.x; x++) {
			// floors/ceilings
			for(size_t i = 0; i < 2; i++) {
				const unsigned int* tiles = (i == 0 ? floor_tiles : ceiling_tiles);
				if(tiles[y*map_size.x + x] > 0) {
					const labdata::lab_floor* tile_data = lab_data->get_floor(tiles[y*map_size.x + x]);
					unsigned int fc_index = fc_static_num*4;
					if(tile_data->animation > 1) {
						fc_index = fc_ani_offset + fc_ani_num*4;
						fc_ani_num++;
						animated_tiles.push_back(make_tuple(0, tiles[y*map_size.x + x], uint2(x, y)));
					}
					else fc_static_num++;

					const float& theight = (i == 0 ? floor_height : ceiling_height);
					fc_vertices[fc_index + 0].set(float(x)*tile_size, theight, float(y)*tile_size);
					fc_vertices[fc_index + 1].set(float(x)*tile_size + tile_size, theight, float(y)*tile_size);
					fc_vertices[fc_index + 2].set(float(x)*tile_size + tile_size, theight, float(y)*tile_size + tile_size);
					fc_vertices[fc_index + 3].set(float(x)*tile_size, theight, float(y)*tile_size + tile_size);
					
					const float2& tc_b = tile_data->tex_info[0].tex_coord_begin;
					const float2& tc_e = tile_data->tex_info[0].tex_coord_end;
					fc_tex_coords[fc_index + 0].set(tc_b.x, tc_b.y);
					fc_tex_coords[fc_index + 1].set(tc_e.x, tc_b.y);
					fc_tex_coords[fc_index + 2].set(tc_e.x, tc_e.y);
					fc_tex_coords[fc_index + 3].set(tc_b.x, tc_e.y);

					const uint2 idx_order = (i == 0 ? uint2(2, 0) : uint2(0, 2));
					fc_indices[0][fc_num*2].set(fc_index + idx_order.x, fc_index + 1, fc_index + idx_order.y);
					fc_indices[0][fc_num*2 + 1].set(fc_index + idx_order.y, fc_index + 3, fc_index + idx_order.x);
					fc_num++;
				}
			}
			// walls
			if(ow_tiles[y*map_size.x + x] >= 101) {
				const labdata::lab_wall* tile_data = lab_data->get_wall(ow_tiles[y*map_size.x + x]);
				if(tile_data == nullptr) continue;
				
				const unsigned char& side = wall_sides[y*map_size.x + x];
				unsigned int side_count = 0;
				for(size_t i = 0; i < 4; i++) {
					if((side & (unsigned char)(1 << i)) != 0) side_count++;
				}
				
				bool alpha_wall = (tile_data->type & labdata::WT_CUT_ALPHA);
				bool transp_wall = false;
				if(tile_data->type & labdata::WT_TRANSPARENT) {
					wall_subobj+=2;
					transp_wall = true;
					alpha_wall = true;
					wall_indices[wall_subobj-1] = new uint3[side_count*2];
					wall_index_count[wall_subobj-1] = side_count*2;
					wall_indices[wall_subobj] = new uint3[side_count*2];
					wall_index_count[wall_subobj] = side_count*2;
				}
				
				if(side_count == 0) continue;

				unsigned int wall_index = wall_static_num*4;
				if(tile_data->animation > 1) {
					wall_index = wall_ani_offset + wall_ani_num*4;
					wall_ani_num+=side_count;
					animated_tiles.push_back(make_tuple(1, ow_tiles[y*map_size.x + x], uint2(x, y)));
					if(alpha_wall) {
						wall_ani_num+=side_count;
					}
				}
				else {
					wall_static_num+=side_count;
					if(alpha_wall) {
						wall_static_num+=side_count;
					}
				}

				const float y_size = map_scale.y * float(tile_data->y_size)/8.0f;

				const float2& tc_b = tile_data->tex_coord_begin[0];
				const float2& tc_e = tile_data->tex_coord_end[0];
				for(size_t alpha_side = 0; alpha_side < (alpha_wall ? 2 : 1); alpha_side++) {
					unsigned int wall_transp_num = 0;
					for(size_t i = 0; i < 4; i++) {
						// only add necessary walls
						if((side & (unsigned char)(1 << i)) == 0) continue;
						
						switch(i) {
							case 0: // north
								wall_vertices[wall_index + 0].set(float(x)*tile_size, y_size, float(y)*tile_size);
								wall_vertices[wall_index + 1].set(float(x)*tile_size, floor_height, float(y)*tile_size);
								wall_vertices[wall_index + 2].set(float(x)*tile_size + tile_size, y_size, float(y)*tile_size);
								wall_vertices[wall_index + 3].set(float(x)*tile_size + tile_size, floor_height, float(y)*tile_size);
								break;
							case 1: // east
								wall_vertices[wall_index + 0].set(float(x)*tile_size + tile_size, y_size, float(y)*tile_size);
								wall_vertices[wall_index + 1].set(float(x)*tile_size + tile_size, floor_height, float(y)*tile_size);
								wall_vertices[wall_index + 2].set(float(x)*tile_size + tile_size, y_size, float(y)*tile_size + tile_size);
								wall_vertices[wall_index + 3].set(float(x)*tile_size + tile_size, floor_height, float(y)*tile_size + tile_size);
								break;
							case 2: // south
								wall_vertices[wall_index + 0].set(float(x)*tile_size + tile_size, y_size, float(y)*tile_size + tile_size);
								wall_vertices[wall_index + 1].set(float(x)*tile_size + tile_size, floor_height, float(y)*tile_size + tile_size);
								wall_vertices[wall_index + 2].set(float(x)*tile_size, y_size, float(y)*tile_size + tile_size);
								wall_vertices[wall_index + 3].set(float(x)*tile_size, floor_height, float(y)*tile_size + tile_size);
								break;
							case 3: // west
								wall_vertices[wall_index + 0].set(float(x)*tile_size, y_size, float(y)*tile_size + tile_size);
								wall_vertices[wall_index + 1].set(float(x)*tile_size, floor_height, float(y)*tile_size + tile_size);
								wall_vertices[wall_index + 2].set(float(x)*tile_size, y_size, float(y)*tile_size);
								wall_vertices[wall_index + 3].set(float(x)*tile_size, floor_height, float(y)*tile_size);
								break;
							default: break;
						}
						
						wall_tex_coords[wall_index + 0].set(tc_b.x, tc_e.y);
						wall_tex_coords[wall_index + 1].set(tc_e.x, tc_e.y);
						wall_tex_coords[wall_index + 2].set(tc_b.x, tc_b.y);
						wall_tex_coords[wall_index + 3].set(tc_e.x, tc_b.y);
						
						// draw both sides of an transparent or alpha tested wall
						if(transp_wall) {
							if(alpha_side == 0) {
								// side #1
								wall_indices[wall_subobj-1][wall_transp_num*2].set(wall_index + 1, wall_index + 0, wall_index + 2);
								wall_indices[wall_subobj-1][wall_transp_num*2 + 1].set(wall_index + 2, wall_index + 3, wall_index + 1);
							}
							else {
								// side #2
								wall_indices[wall_subobj][wall_transp_num*2].set(wall_index + 2, wall_index + 0, wall_index + 1);
								wall_indices[wall_subobj][wall_transp_num*2 + 1].set(wall_index + 1, wall_index + 3, wall_index + 2);
							}
							wall_transp_num++;
						}
						else {
							if(alpha_side == 0) {
								wall_indices[0][wall_num*2].set(wall_index + 1,
																wall_index + 0,
																wall_index + 2);
								wall_indices[0][wall_num*2 + 1].set(wall_index + 2,
																	wall_index + 3,
																	wall_index + 1);
							}
							else {
								wall_indices[0][wall_num*2].set(wall_index + 0,
																wall_index + 1,
																wall_index + 2);
								wall_indices[0][wall_num*2 + 1].set(wall_index + 1,
																	wall_index + 3,
																	wall_index + 2);
							}
							wall_num++;
						}
						
						wall_index+=4;
					}
				}
			}
			// objects
			if(ow_tiles[y*map_size.x + x] > 0 && ow_tiles[y*map_size.x + x] < 101) {
				const labdata::lab_object* obj = lab_data->get_object(ow_tiles[y*map_size.x + x]);
				if(obj == nullptr) continue;

				unsigned int object_index = object_num*4;
				if(obj->animated) {
					object_index = obj_ani_offset + obj_ani_num*4;
					obj_ani_num += obj->sub_object_count;
					animated_tiles.push_back(make_tuple(2, ow_tiles[y*map_size.x + x], uint2(x, y)));
				}
				else object_num += obj->sub_object_count;

				for(size_t i = 0; i < obj->sub_object_count; i++) {
					//
					const labdata::lab_object_info* sub_object = obj->sub_objects[i];
					//const float2 obj_size = float2(sub_object->x_size, sub_object->y_size);
					//const float2 obj_scale = float2(sub_object->x_scale, sub_object->y_scale);
					const float2& tc_b = sub_object->tex_coord_begin[0];
					const float2& tc_e = sub_object->tex_coord_end[0];
					const float x_size = float(sub_object->x_scale)/32.0f;
					const float y_size = float(sub_object->y_scale)/32.0f;
					float3 offset = float3(obj->offset[i].x, -obj->offset[i].y, obj->offset[i].z)/32.0f;

					if(sub_object->type & 0x4) {
						if(obj->offset[i].z == 0) {
							obj_vertices[object_index + 0].set((-x_size/2.0f), 0.001f, (-y_size/2.0f));
							obj_vertices[object_index + 1].set((-x_size/2.0f), 0.001f, (y_size/2.0f));
							obj_vertices[object_index + 2].set((x_size/2.0f), 0.001f, (-y_size/2.0f));
							obj_vertices[object_index + 3].set((x_size/2.0f), 0.001f, (y_size/2.0f));
						}
						else {
							obj_vertices[object_index + 0].set((x_size/2.0f), 0.0f, (-y_size/2.0f));
							obj_vertices[object_index + 1].set((x_size/2.0f), 0.0f, (y_size/2.0f));
							obj_vertices[object_index + 2].set((-x_size/2.0f), 0.0f, (-y_size/2.0f));
							obj_vertices[object_index + 3].set((-x_size/2.0f), 0.0f, (y_size/2.0f));
						}
					}
					else {
						obj_vertices[object_index + 0].set((-x_size/2.0f), (y_size/2.0f), 0.0f);
						obj_vertices[object_index + 1].set((-x_size/2.0f), (-y_size/2.0f), 0.0f);
						obj_vertices[object_index + 2].set((x_size/2.0f), (y_size/2.0f), 0.0f);
						obj_vertices[object_index + 3].set((x_size/2.0f), (-y_size/2.0f), 0.0f);
						offset.z += (y_size/2.0f);
					}

					obj_ws_positions[object_index + 0] = float3(float(x)*tile_size + offset.x, offset.z, offset.y + float(y+1)*tile_size);
					obj_ws_positions[object_index + 1] = float3(float(x)*tile_size + offset.x, offset.z, offset.y + float(y+1)*tile_size);
					obj_ws_positions[object_index + 2] = float3(float(x)*tile_size + offset.x, offset.z, offset.y + float(y+1)*tile_size);
					obj_ws_positions[object_index + 3] = float3(float(x)*tile_size + offset.x, offset.z, offset.y + float(y+1)*tile_size);
					
					obj_tex_coords[object_index + 0].set(tc_e.x, tc_b.y);
					obj_tex_coords[object_index + 1].set(tc_e.x, tc_e.y);
					obj_tex_coords[object_index + 2].set(tc_b.x, tc_b.y);
					obj_tex_coords[object_index + 3].set(tc_b.x, tc_e.y);
				
					obj_indices[0][obj_static_num*2].set(object_index + 1, object_index + 0, object_index + 2);
					obj_indices[0][obj_static_num*2 + 1].set(object_index + 2, object_index + 3, object_index + 1);

					obj_static_num++;
					object_index+=4;
				}
			}
		}
	}
	
	//cout << ":: #triangles: " << (tile_count*2) << endl;
	//cout << ":: #obj-triangles: " << (sub_object_count*2) << endl;

	fc_tiles_model = new map_tiles();
	fc_tiles_model->load_from_memory(1, fc_count*4, fc_vertices, fc_tex_coords, fc_index_count, fc_indices);
	fc_tiles_model->set_material(lab_data->get_fc_material());
	sce->add_model(fc_tiles_model);

	wall_model = new map_tiles();
	wall_model->load_from_memory(1+wall_transp_obj_count, wall_count*4, wall_vertices, wall_tex_coords, wall_index_count, wall_indices);
	wall_model->set_material(lab_data->get_wall_material());
	sce->add_model(wall_model);
	
	// make wall sub-objects transparent
	vector<size_t> transp_subobjs;
	for(size_t i = 0; i < wall_transp_obj_count; i++) {
		wall_model->set_transparent(1+i, true);
		transp_subobjs.push_back(1+i);
	}
	lab_data->get_wall_material()->copy_object_mapping(0, transp_subobjs);
	
	objects_model = new map_objects();
	objects_model->load_from_memory(1, sub_object_count*4, obj_vertices, obj_tex_coords, obj_index_count, obj_indices);
	objects_model->set_ws_positions(obj_ws_positions);
	objects_model->set_material(lab_data->get_object_material());
	sce->add_model(objects_model);

	// don't delete model data, these are taken care of by a2estatic/a2emodel now!
	
	
	if(conf::get<bool>("map.3d.object_lights")) {
		// add lights specific for this map and labdata
		const auto& light_objects = lab_data->get_light_objects();
		if(light_objects.count((unsigned int)cur_labdata_num) > 0) {
			const auto& light_objs = light_objects.find((unsigned int)cur_labdata_num)->second;
			
			// objects
			const float half_tile_size = tile_size * 0.5f;
			for(size_t y = 0; y < map_size.y; y++) {
				for(size_t x = 0; x < map_size.x; x++) {
					const unsigned int& tile_obj = ow_tiles[y*map_size.x + x];
					if(tile_obj > 0 && tile_obj < 101) {
						const auto& lobj = light_objs->find(tile_obj);
						if(lobj != light_objs->end()) {
							// TODO: y dependent on map height
							// use ceiling_height for now
							object_lights.push_back(object_light_base::create(lobj->second, float3(float(x)*tile_size + half_tile_size, ceiling_height * 0.8f, float(y)*tile_size + half_tile_size)));
						}
					}
				}
			}
			
			// npcs
			for(auto& n : npcs) {
				const unsigned int obj_num = n->get_object_num();
				if(obj_num == 0) continue;
				const auto& lobj = light_objs->find(obj_num);
				if(lobj != light_objs->end()) {
					// TODO: look above
					object_light_base* l = object_light_base::create(lobj->second, float3(0.0f, ceiling_height * 0.8f, 0.0f));
					l->track(n);
					l->set_enabled(n->is_enabled());
					object_lights.push_back(l);
				}
			}
		}
		cout << "#lights: " << object_lights.size() << endl;
	}
	object_light_ani_time = SDL_GetTicks();
	sun_distance = (float(map_size.x) / 2.0f) * std_tile_size;
	
	// render these at the end (faster since it'll only write the pixels that are really necessary)
	sce->add_model(npcs_model);
	sce->add_model(bg3d);

	// and finally:
	map_loaded = true;
	cur_map_num = map_num;
	log_debug("map #%u loaded!", map_num);
}

void map3d::unload() {
	if(wall_model != nullptr) {
		sce->delete_model(wall_model);
		delete wall_model;
		wall_model = nullptr;
		
		// can be nullptred now
		wall_vertices = nullptr;
		wall_tex_coords = nullptr;
		wall_indices = nullptr;
	}
	if(fc_tiles_model != nullptr) {
		sce->delete_model(fc_tiles_model);
		delete fc_tiles_model;
		fc_tiles_model = nullptr;
		
		// can be nullptred now
		fc_vertices = nullptr;
		fc_tex_coords = nullptr;
		fc_indices = nullptr;
	}
	if(objects_model != nullptr) {
		sce->delete_model(objects_model);
		delete objects_model;
		objects_model = nullptr;
		
		// can be nullptred now
		obj_vertices = nullptr;
		obj_ws_positions = nullptr;
		obj_tex_coords = nullptr;
		obj_indices = nullptr;
	}
	if(ow_tiles != nullptr) {
		delete ow_tiles;
		ow_tiles = nullptr;
	}
	if(floor_tiles != nullptr) {
		delete floor_tiles;
		floor_tiles = nullptr;
	}
	if(ceiling_tiles != nullptr) {
		delete ceiling_tiles;
		ceiling_tiles = nullptr;
	}
	if(npcs_model != nullptr) {
		sce->delete_model(npcs_model);
		delete npcs_model;
		npcs_model = nullptr;
		
		// can be nullptred now
		npcs_vertices = nullptr;
		npcs_ws_positions = nullptr;
		npcs_tex_coords = nullptr;
		npcs_indices = nullptr;
		npc_object_count = 0;
	}
	if(wall_sides != nullptr) {
		delete [] wall_sides;
		wall_sides = nullptr;
	}
	
	//
	for(vector<npc3d*>::iterator npc_iter = npcs.begin(); npc_iter != npcs.end(); npc_iter++) {
		delete *npc_iter;
	}
	npcs.clear();
	if(mnpcs != nullptr) {
		delete mnpcs;
		mnpcs = nullptr;
	}
	
	for(auto& ol : object_lights) {
		delete ol;
	}
	object_lights.clear();
	
	mevents.unload();
	map_loaded = false;

	last_tile_animation = SDL_GetTicks();
	fc_ani_count = 0;
	fc_ani_offset = 0;
	wall_ani_count = 0;
	wall_ani_offset = 0;
	obj_ani_count = 0;
	obj_ani_offset = 0;
	animated_tiles.clear();
	cur_map_num = (~0ull);

	if(bg_loaded) {
		sce->delete_model(bg3d);
		bg_loaded = false;
	}
}

void map3d::draw() const {
	if(!map_loaded) return;
}

void map3d::clock_tick(size_t ticks) {
	if(!active_sun_light) return;
	
	// sun movement and coloring
	static const size_t sun_rise_hour = 7;
	static const size_t sun_set_hour = 21;
	static const size_t sun_rise = sun_rise_hour*AR_TICKS_PER_HOUR;
	static const size_t sun_set = sun_set_hour*AR_TICKS_PER_HOUR;
	static const size_t sun_up = sun_set - sun_rise;
	static const float fsun_up = float(sun_up);
	if(ticks >= sun_rise && ticks <= sun_set) {
		sun_light->set_enabled(true);
		
		// we're moving on top of the x axis
		// also: let's just pretend we're on the equator
		// and position the sun at one end of the map
		const size_t progression = ticks - sun_rise;
		const float norm_progression = float(progression) / fsun_up;
		sun_light->set_position((sun_distance - norm_progression * sun_distance) * 2.0f,
								sinf(const_math::deg_to_rad(norm_progression * 180.0f)) * sun_distance,
								float(map_size.y) * std_tile_size);
		
		// set sun light color
		// intensity = 1 - (x - 0.5)^2 / 0.8
		const float intensity = 1.0f - (norm_progression - 0.5f)*(norm_progression - 0.5f)*1.25f;
		const float3 sun_light_color = float3(intensity * sun_color_table[size_t(norm_progression * float(sun_color_table.size()))]).clamped(0.0f, 1.0f);
		sun_light->set_color(sun_light_color);
		sun_light->set_ambient(sun_light_color * 0.25f);
		bg3d->set_light_color(sun_light_color);
	}
	else {
		bg3d->set_light_color(float3(0.0f));
		sun_light->set_enabled(false);
	}
}

void map3d::handle() {
	if(!map_loaded) return;

	// attach player light to camera for now
	player_light->set_position(-float3(*engine::get_position()));
	
	// update/animate object lights
	const unsigned int& ani_time_diff = SDL_GetTicks() - object_light_ani_time;
	if(ani_time_diff > 0) {
		object_light_ani_time += ani_time_diff;
		for(auto& ol : object_lights) {
			ol->handle();
			ol->animate(ani_time_diff);
		}
	}
	
	// handle npcs
	const vector<map_npcs::map_npc*>& npc_data = mnpcs->get_npcs();
	size_t npc_num = 0, npc_index = 0;
	for(vector<npc3d*>::iterator npc_iter = npcs.begin(); npc_iter != npcs.end(); npc_iter++) {
		(*npc_iter)->handle();
		
		const labdata::lab_object* obj = lab_data->get_object(npc_data[npc_num]->object_num);
		for(size_t i = 0; i < obj->sub_object_count; i++) {
			const float y_size = float(obj->sub_objects[i]->y_scale)/32.0f;
			float3 offset = float3(obj->offset[i].x, -obj->offset[i].y, obj->offset[i].z)/32.0f;
			offset.z += (y_size/2.0f);
			float2 npc_pos = npcs[npc_num]->get_interpolated_pos();
			
			npcs_ws_positions[npc_index + 0] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			npcs_ws_positions[npc_index + 1] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			npcs_ws_positions[npc_index + 2] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			npcs_ws_positions[npc_index + 3] = float3(npc_pos.x*tile_size + offset.x, offset.z, offset.y + (npc_pos.y+1.0f)*tile_size);
			npc_index+=4;
		}
		npc_num++;
	}
	glBindBuffer(GL_ARRAY_BUFFER, npcs_model->get_vbo_ws_position_id());
	glBufferSubData(GL_ARRAY_BUFFER, 0, (npc_object_count * 4) * 3 * sizeof(float), npcs_ws_positions);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// handle animations
	if((SDL_GetTicks() - last_tile_animation) > TIME_PER_TILE_ANIMATION_FRAME) {
		last_tile_animation = SDL_GetTicks();

		lab_data->handle_animations();
		
		// map/tiles
		//size_t index = 0, ani_num = 0;
		size_t fc_index = 0, fc_ani_num = 0;
		size_t wall_index = 0, wall_ani_num = 0;
		size_t obj_index = 0, obj_ani_num = 0;
		for(auto ani_tile_iter = animated_tiles.begin(); ani_tile_iter != animated_tiles.end(); ani_tile_iter++) {
			if(get<0>(*ani_tile_iter) == 0) {
				fc_index = fc_ani_offset + fc_ani_num*4;
				fc_ani_num++;
			}
			else if(get<0>(*ani_tile_iter) == 1) {
				wall_index = wall_ani_offset + wall_ani_num*4;
				wall_ani_num++;
			}
			else if(get<0>(*ani_tile_iter) == 2) {
				obj_index = obj_ani_offset + obj_ani_num*4;
			}

			switch(get<0>(*ani_tile_iter)) {
				case 0: {
					const labdata::lab_floor* tile_data = lab_data->get_floor(get<1>(*ani_tile_iter));
					const float2& tc_b = tile_data->tex_info[tile_data->cur_ani].tex_coord_begin;
					const float2& tc_e = tile_data->tex_info[tile_data->cur_ani].tex_coord_end;
					fc_tex_coords[fc_index + 0].set(tc_b.x, tc_b.y);
					fc_tex_coords[fc_index + 1].set(tc_e.x, tc_b.y);
					fc_tex_coords[fc_index + 2].set(tc_e.x, tc_e.y);
					fc_tex_coords[fc_index + 3].set(tc_b.x, tc_e.y);
				}
				break;
				case 1: {
					const labdata::lab_wall* tile_data = lab_data->get_wall(get<1>(*ani_tile_iter));
					if(tile_data == nullptr) break;
					const float2& tc_b = tile_data->tex_coord_begin[tile_data->cur_ani];
					const float2& tc_e = tile_data->tex_coord_end[tile_data->cur_ani];
					
					const uint2& pos = get<2>(*ani_tile_iter);
					const unsigned char& side = wall_sides[pos.y*map_size.x + pos.x];
					size_t side_count = 0;
					for(size_t i = 0; i < 4; i++) {
						if((side & (unsigned char)(1 << i)) != 0) side_count++;
					}
					if(side_count == 0) {
						wall_ani_num--; // decrease already increased ani num
						continue;
					}
					else wall_ani_num += side_count-1; // already inc'ed by one
					
					bool alpha_wall = (tile_data->type & (labdata::WT_CUT_ALPHA | labdata::WT_TRANSPARENT));
					if(alpha_wall) wall_ani_num += side_count;
					
					for(size_t alpha_side = 0; alpha_side < (alpha_wall ? 2 : 1); alpha_side++) {
						for(size_t i = 0; i < 4; i++) {
							// only add necessary walls
							if((side & (unsigned char)(1 << i)) == 0) continue;
							
							wall_tex_coords[wall_index + 0].set(tc_b.x, tc_e.y);
							wall_tex_coords[wall_index + 1].set(tc_e.x, tc_e.y);
							wall_tex_coords[wall_index + 2].set(tc_b.x, tc_b.y);
							wall_tex_coords[wall_index + 3].set(tc_e.x, tc_b.y);
							
							wall_index+=4;
						}
					}
				}
				break;
				case 2: {
					const labdata::lab_object* tile_data = lab_data->get_object(get<1>(*ani_tile_iter));

					for(size_t i = 0; i < tile_data->sub_object_count; i++) {
						const float2& tc_b = tile_data->sub_objects[i]->tex_coord_begin[tile_data->sub_objects[i]->cur_ani];
						const float2& tc_e = tile_data->sub_objects[i]->tex_coord_end[tile_data->sub_objects[i]->cur_ani];

						obj_tex_coords[obj_index + 0].set(tc_e.x, tc_b.y);
						obj_tex_coords[obj_index + 1].set(tc_e.x, tc_e.y);
						obj_tex_coords[obj_index + 2].set(tc_b.x, tc_b.y);
						obj_tex_coords[obj_index + 3].set(tc_b.x, tc_e.y);
						
						obj_index+=4;
					}

					obj_ani_num+=tile_data->sub_object_count;
				}
				break;
				default: break;
			}
		}
		
		glBindBuffer(GL_ARRAY_BUFFER, fc_tiles_model->get_vbo_tex_coords());
		glBufferSubData(GL_ARRAY_BUFFER, fc_ani_offset * 2 * sizeof(float), (fc_ani_count * 4) * 2 * sizeof(float), &fc_tex_coords[fc_ani_offset]);
		
		glBindBuffer(GL_ARRAY_BUFFER, wall_model->get_vbo_tex_coords());
		glBufferSubData(GL_ARRAY_BUFFER, wall_ani_offset * 2 * sizeof(float), (wall_ani_count * 4) * 2 * sizeof(float), &wall_tex_coords[wall_ani_offset]);

		glBindBuffer(GL_ARRAY_BUFFER, objects_model->get_vbo_tex_coords());
		glBufferSubData(GL_ARRAY_BUFFER, obj_ani_offset * 2 * sizeof(float), (obj_ani_count * 4) * 2 * sizeof(float), &obj_tex_coords[obj_ani_offset]);
		
		// npcs
		npc_num = 0;
		npc_index = 0;
		for(vector<npc3d*>::iterator npc_iter = npcs.begin(); npc_iter != npcs.end(); npc_iter++) {
			const labdata::lab_object* npc_obj = lab_data->get_object(npc_data[npc_num]->object_num);
			
			for(size_t i = 0; i < npc_obj->sub_object_count; i++) {
				const float2& tc_b = npc_obj->sub_objects[i]->tex_coord_begin[npc_obj->sub_objects[i]->cur_ani];
				const float2& tc_e = npc_obj->sub_objects[i]->tex_coord_end[npc_obj->sub_objects[i]->cur_ani];
				
				npcs_tex_coords[npc_index + 0].set(tc_e.x, tc_b.y);
				npcs_tex_coords[npc_index + 1].set(tc_e.x, tc_e.y);
				npcs_tex_coords[npc_index + 2].set(tc_b.x, tc_b.y);
				npcs_tex_coords[npc_index + 3].set(tc_b.x, tc_e.y);
				npc_index+=4;
			}
			npc_num++;
		}
		
		glBindBuffer(GL_ARRAY_BUFFER, npcs_model->get_vbo_tex_coords());
		glBufferSubData(GL_ARRAY_BUFFER, 0, (npc_object_count * 4) * 2 * sizeof(float), &npcs_tex_coords[0]);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
}

const ssize3 map3d::get_tile() const {
	ssize3 ret(-1);
	const float3 cam_pos = -float3(engine::get_position());
	if(cam_pos.x >= 0.0f && cam_pos.x < tile_size*map_size.x &&
		cam_pos.z >= 0.0f && cam_pos.z < tile_size*map_size.y) {
		size2 cur_pos = float2(cam_pos.x, cam_pos.z)/tile_size;
		ret = ssize3(floor_tiles[cur_pos.y*map_size.x + cur_pos.x], ceiling_tiles[cur_pos.y*map_size.x + cur_pos.x], ow_tiles[cur_pos.y*map_size.x + cur_pos.x]);
	}
	return ret;
}

bool4 map3d::collide(const MOVE_DIRECTION& direction, const size2& cur_position, const CHARACTER_TYPE& char_type) const {
	if(!map_loaded) return bool4(false);
	
	if(!conf::get<bool>("map.collision") && char_type == CHARACTER_TYPE::PLAYER) return bool4(false);
	
	if(cur_position.x >= map_size.x) return bool4(true);
	if(cur_position.y >= map_size.y) return bool4(true);
	
	bool4 ret(false);
	static const size_t max_pos_count = 3;
	ssize2 next_position[max_pos_count];
	next_position[0].x = next_position[0].y = -1;
	next_position[1].x = next_position[1].y = -1;
	next_position[2].x = next_position[2].y = -1;
	size_t position_count = 0; // count == 1 if single direction, == 3 if sideways
	
	if(!((direction & MOVE_DIRECTION::LEFT) != MOVE_DIRECTION::NONE && (direction & MOVE_DIRECTION::RIGHT) != MOVE_DIRECTION::NONE)) {
		if((direction & MOVE_DIRECTION::LEFT) != MOVE_DIRECTION::NONE) {
			if(cur_position.x == 0) {
				ret.x = true;
				ret.y = true;
			}
			else {
				next_position[0] = size2(cur_position.x-1, cur_position.y);
				position_count++;
			}
		}
		else if((direction & MOVE_DIRECTION::RIGHT) != MOVE_DIRECTION::NONE) {
			if(cur_position.x+1 >= map_size.x) {
				ret.x = true;
				ret.y = true;
			}
			else {
				next_position[0] = size2(cur_position.x+1, cur_position.y);
				position_count++;
			}
		}
	}
	
	if(!((direction & MOVE_DIRECTION::UP) != MOVE_DIRECTION::NONE && (direction & MOVE_DIRECTION::DOWN) != MOVE_DIRECTION::NONE)) {
		if((direction & MOVE_DIRECTION::UP) != MOVE_DIRECTION::NONE) {
			if(cur_position.y == 0) {
				ret.x = true;
				ret.z = true;
			}
			else {
				next_position[1] = size2(cur_position.x, cur_position.y-1);
				position_count++;
			}
		}
		else if((direction & MOVE_DIRECTION::DOWN) != MOVE_DIRECTION::NONE) {
			if(cur_position.y+1 >= map_size.y) {
				ret.x = true;
				ret.z = true;
			}
			else {
				next_position[1] = size2(cur_position.x, cur_position.y+1);
				position_count++;
			}
		}
	}
	
	// check for sideways movement
	if(position_count == 2) {
		if((direction & MOVE_DIRECTION::LEFT) != MOVE_DIRECTION::NONE && (direction & MOVE_DIRECTION::UP) != MOVE_DIRECTION::NONE) {
			next_position[2] = size2(cur_position.x-1, cur_position.y-1);
			position_count++;
		}
		else if((direction & MOVE_DIRECTION::LEFT) != MOVE_DIRECTION::NONE && (direction & MOVE_DIRECTION::DOWN) != MOVE_DIRECTION::NONE) {
			next_position[2] = size2(cur_position.x-1, cur_position.y+1);
			position_count++;
		}
		else if((direction & MOVE_DIRECTION::RIGHT) != MOVE_DIRECTION::NONE && (direction & MOVE_DIRECTION::UP) != MOVE_DIRECTION::NONE) {
			next_position[2] = size2(cur_position.x+1, cur_position.y-1);
			position_count++;
		}
		else if((direction & MOVE_DIRECTION::RIGHT) != MOVE_DIRECTION::NONE && (direction & MOVE_DIRECTION::DOWN) != MOVE_DIRECTION::NONE) {
			next_position[2] = size2(cur_position.x+1, cur_position.y+1);
			position_count++;
		}
		
		if(position_count != max_pos_count) {
			log_error("impossible movement!");
			return bool4(true);
		}
	}
	
	//
	for(size_t i = 0; i < max_pos_count; i++) {
		if(next_position[i].x == -1 && next_position[i].y == -1) {
			ret[1+i] = false;
			continue;
		}
		
		const size_t index = size_t(next_position[i].y) * map_size.x + size_t(next_position[i].x);
		
		const labdata::lab_floor* floor_data = nullptr;
		const labdata::lab_floor* ceiling_data = nullptr;
		const labdata::lab_wall* wall_data = nullptr;
		const labdata::lab_object* obj_data = nullptr;
		
		if(floor_tiles[index] > 0) floor_data = lab_data->get_floor(floor_tiles[index]);
		if(ceiling_tiles[index] > 0) ceiling_data = lab_data->get_floor(ceiling_tiles[index]);
		if(ow_tiles[index] >= 101) wall_data = lab_data->get_wall(ow_tiles[index]);
		// TODO: add correct object collision
		//if(ow_tiles[index] > 0 && ow_tiles[index] < 101) obj_data = lab_data->get_object(ow_tiles[index]);
		
		if(floor_data != nullptr) {
			if(floor_data->collision & labdata::CT_BLOCK) {
				ret[1+i] = true;
			}
		}
		if(ceiling_data != nullptr) {
			if(ceiling_data->collision & labdata::CT_BLOCK) {
				ret[1+i] = true;
			}
		}
		if(wall_data != nullptr) {
			if(wall_data->collision & labdata::CT_BLOCK) {
				ret[1+i] = true;
			}
		}
		if(obj_data != nullptr) {
			for(size_t so = 0; so < obj_data->sub_object_count; so++) {
				if(obj_data->sub_objects[so]->collision & labdata::CT_BLOCK) {
					ret[1+i] = true;
				}
			}
		}
		
		if(ret[1+i]) ret[0] = true;
	}

	return ret;
}

map_events& map3d::get_map_events() {
	return mevents;
}

labdata* map3d::get_active_lab_data() const {
	return lab_data;
}
