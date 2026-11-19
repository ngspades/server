// Copyright DarkNeutrino 2021
#ifndef MAP_H
#define MAP_H

#include <Server/Structs/ServerStruct.h>
#include <Util/Queue.h>
#include <Util/Types.h>

uint8_t map_load(server_t* server, const char* path, int map_size[3]);
/* a folder of maps searched before Resources/maps (--maps-dir: the listen
   server's player keeps theirs in it); NULL or "" for none */
void map_set_user_dir(const char* dir);
const char* map_user_dir_get(void);
/* <Name>/<Name>.<ext> in the user folder when its .vxl is there, else in
   Resources/maps when it is there, else in the data folder's maps/ (a map
   downloaded where Resources/ is read only, MapDownload.c), else the
   Resources/maps path (for the messages) */
void map_file_path(char* out, size_t size, const char* name, const char* ext);
int  map_ngmap_folder(char* out, size_t size, const char* name);

#endif
