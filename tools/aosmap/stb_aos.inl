// AoS map generation helper functions
//
// Modified for ngspades: the map size is a compile-time option;
// numbers that described a 512 map are fractions of it.

#ifdef _MSC_VER
#pragma warning(disable:4244; disable:4305; disable:4101)
#endif

#define STB_DEFINE
#include "stb.h"   // http://nothings.org/stb.h

// define the main map structure: -DMAP_X=1024 -DMAP_Y=1024 builds a 1024
// map (the generators put static arrays of this size in the binary)
#ifndef MAP_X
#define MAP_X  512
#endif
#ifndef MAP_Y
#define MAP_Y  512
#endif
#define MAP_Z   64

// when building geometrys, we'll always let them wraparound
// on the map so they're seamless at the map edges
#define MASK_X  (MAP_X-1)
#define MASK_Y  (MAP_Y-1)

// a length (or a position) that described a 512 x 512 map, as a fraction of
// this one: MAP_XS(160) is 160 at 512 and 320 at 1024. The generators keep
// their features at the same size and their regions at the same place, so a
// bigger map is the same kind of map, bigger.
#define MAP_XS(n)  ((n) * MAP_X / 512)
#define MAP_YS(n)  ((n) * MAP_Y / 512)

// voxel data
uint8 map[MAP_X][MAP_Y][MAP_Z];
uint32 color[MAP_X][MAP_Y][MAP_Z]; // MAP_X*MAP_Y*MAP_Z*4 bytes: 64 MB at 512, 256 MB at 1024

// 2d data while generating

float data[MAP_X][MAP_Y];
float temp[MAP_X][MAP_Y];
float temp2[MAP_X][MAP_Y];

int height[MAP_X][MAP_Y];
int debug;



///////////////////////////////////////////////////////////////////////////////////////
//
//  general-purpose tools for building maps
//

static int rnd(int x, int y)
{
   assert(x <= y);
   return stb_rand() % (y-x+1) + x;
}

static float smoothstep(float v)
{
   if (v < 0) return 0;
   if (v > 1) return 1;
   return 3*v*v - 2*v*v*v;
}

static float smooth_squarestep(float v)
{
   if (v < 0) return 0;
   if (v > 1) return 1;
   return v*v;
}

static float smooth_cubestep(float v)
{
   if (v < 0) return 0;
   if (v > 1) return 1;
   return v*v*v;
}

// 2d tools

float stb_perlin_noise3(float x, float y, float z, int x_wrap, int y_wrap, int z_wrap);

// generate perlin noise into a grid
// samples is the "number of samples" if you imagined subdividing it to a grid, must be power of two
// w is a 'peakiness' factor that from 0 to 1 weights between the noise value being linear, and being cubed
void grid_random(float dest[MAP_X][MAP_Y], int x_samples, int y_samples, float r0, float r1, float w, float x_origin, float y_origin, float z)
{
   int i,j;
   int x_wrap = x_samples;
   int y_wrap = y_samples;
   float x_scale = (float) x_wrap / MAP_X;
   float y_scale = (float) y_wrap / MAP_Y;

   // now go back through and bilerp them
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         float x = x_scale * i + x_origin;
         float y = y_scale * j + y_origin;
         float p = stb_perlin_noise3(x,y,z, x_wrap,y_wrap,0);
         // p is a bit larger than -1 to 1
         p /= 1.2f;
         p = (p + 1) / 2;
         p = stb_lerp(w, p, p*p*p);
         dest[i][j] = stb_lerp(p, r0,r1);
      }
   }
}

// like photoshop "offset" filter
void translate(float dest[MAP_X][MAP_Y], float src[MAP_X][MAP_Y], int dx, int dy)
{
   int i,j;
   for (i=0; i < MAP_X; ++i)
      for (j=0; j < MAP_Y; ++j)
         dest[i][j] = src[(i+dx)&MASK_X][(j+dy)&MASK_Y];
}


void grid_addeq_scale(float dest[MAP_X][MAP_Y], float src[MAP_X][MAP_Y], float scale)
{
   int i,j;
   for (i=0; i < MAP_X; ++i)
      for (j=0; j < MAP_Y; ++j)
         dest[i][j] += src[i][j] * scale;
}

void grid_minimize(float dest[MAP_X][MAP_Y])
{
   float f = dest[0][0];
   int i,j;
   for (i=0; i < MAP_X; ++i)
      for (j=0; j < MAP_Y; ++j)
         if (dest[i][j] < f) f = dest[i][j];

   for (i=0; i < MAP_X; ++i)
      for (j=0; j < MAP_Y; ++j)
         dest[i][j] -= f;
}

void grid_limit(float dest[MAP_X][MAP_Y], float limit)
{
   float f = dest[0][0];
   int i,j;
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j)
         if (dest[i][j] > f) f = dest[i][j];
   }

   if (f > limit) {
      float scale = limit / f;
      for (i=0; i < MAP_X; ++i) {
         for (j=0; j < MAP_Y; ++j)
            dest[i][j] *= scale;
      }
   }
}



// voxel tools

void draw_column(int x, int y, int z0, int z1, uint32 c)
{
   int z;
   int solid = (c != 0);
   for (z=z0; z <= z1; ++z) {
      map  [x][y][z] = solid;
      color[x][y][z] = c;
   }
}

void draw_sphere(int x, int y, int z, int r, uint32 c)
{
   int i,j,k;
   int solid = (c != 0);

   // compute r2 to be halfway between r and r+1, so
   // that we don't get little nubbins on our spheres
   int r2 = r*r + ((2*r+1) >> 1);

   for (i= -r; i <= r; ++i) {
      for (j= -r; j <= r; ++j) {
         for (k= -r; k <= r; ++k) {
            if (i*i + j*j + k*k < r*r) {
               if (z+k >= 0 && z+k < MAP_Z) {
                  int a = (x+i) & MASK_X;
                  int b = (y+j) & MASK_Y;
                  map  [a][b][z+k] = solid;
                  color[a][b][z+k] = c;
               }
            }
         }
      }
   }
}

void draw_sphere_slice(int x, int y, int z, int r, uint32 c, int k0, int k1)
{
   int i,j,k;
   int solid = (c != 0);

   // compute r^2 to be halfway between r^2 and (r+1)^2, so
   // that we don't get little nubbins on our spheres
   int r2 = r*r + ((2*r+1) >> 1);

   for (i= -r; i <= r; ++i) {
      for (j= -r; j <= r; ++j) {
         for (k= k0; k <= k1; ++k) {
            if (i*i + j*j + k*k < r*r) {
               if (z+k >= 0 && z+k < MAP_Z) {
                  int a = (x+i) & MASK_X;
                  int b = (y+j) & MASK_Y;
                  map  [a][b][z+k] = solid;
                  color[a][b][z+k] = c;
               }
            }
         }
      }
   }
}

void draw_rotated_ellipse(int x, int y, int z, float theta, float rx, float ry, float rz, uint32 c)
{
   float sn = sin(theta);
   float cs = cos(theta);
   int solid = (c != 0);
   int bounds = stb_max(rx,ry);
   int i,j,k, zb = rz;
   for (k=-rz; k <= rz; ++k) {
      for (i=-bounds; i <= bounds; ++i) {
         for (j = -bounds; j <= bounds; ++j) {
            float dx, dy, dz;
            dx = ( cs * i + sn * j) / rx;
            dy = (-sn * i + cs * j) / ry;
            dz = k / rz;
            if (dx * dx + dy * dy + dz * dz < 1) {
               if (z + k >= 0 && z + k < MAP_Z) {
                  int a = (x+i) & MASK_X;
                  int b = (y+j) & MASK_Y;
                  map[a][b][z+k] = solid;
                  color[a][b][z+k] = c;
               }
            }
         }
      }
   }
}

void draw_partial_sphere_line(int x0, int y0, int z0, int x1, int y1, int z1, int rad, int k0, int k1, uint8 *c0, uint8 *c1, int a)
{
   int i;
   int dx = abs(x0-x1) + 1;
   int dy = abs(y0-y1) + 1;
   int d = stb_max(dx,dy);
   for (i=0; i <= d; ++i) {
      float t = stb_linear_remap(i,0,d, 0.0, 1.0);
      // problem with using linear remap is it doesn't round
      int x = x0 + (x1-x0)*t + 0.5;
      int y = y0 + (y1-y0)*t + 0.5;
      int z = z0 + (z1-z0)*t + 0.5;
      int r = stb_linear_remap(i,0,d, c0[0],c1[0]);
      int g = stb_linear_remap(i,0,d, c0[1],c1[1]);
      int b = stb_linear_remap(i,0,d, c0[2],c1[2]);
      draw_sphere_slice(x, y, z, rad, r*65536+g*256+b + (a << 24), k0,k1);
   }
}




void add_shore_detail(void)
{
   grid_random(temp,   64, 64,  -0.5,0.5,0, 0,0,stb_frand()*64);
   grid_random(temp2, 128, 128,    0,  1,0, 0,0,stb_frand()*64); grid_addeq_scale(temp, temp2, 1);
   grid_random(temp2, 256, 256,    0, -1,0, 0,0,stb_frand()*64); grid_addeq_scale(temp, temp2, 1);
   grid_addeq_scale(data, temp, 1);
}

// this is run AFTER building the height array, so it only affects the lighting computation
void add_lighting_detail(void)
{
   float w1 = 1.0;
   float w2 = 1.0/5;
   float w3 = 1.0/12;
   float scale = 2;
   grid_random(temp,   32,  32, 0,w1, 0, 0,0,stb_frand()*64);
   grid_random(temp2, 128, 128, 0,w2,0, 0,0,stb_frand()*64); grid_addeq_scale(temp, temp2, 1);
   grid_random(temp2, 256, 256, 0,w3,0, 0,0,stb_frand()*64); grid_addeq_scale(temp, temp2, 1); 
   grid_addeq_scale(data, temp, scale);
}

void build_height_array(void)
{
   int i,j,k;
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         int z;
         z = (int) data[i][j];
         if (z < 0) z = 0;
         if (z > 62) z = 62;

         height[i][j] = z;
         for (k=0; k < MAP_Z; ++k)
            map[i][j][k] = (k <= z);
      }
   }
}

void compute_ground_lighting(int green_height, int white_height)
{
   int i,j,k;
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         uint32 c;
         float brightness;
         float height_color = stb_linear_remap(data[i][j], green_height,white_height, 0,1);
         float dx = data[i][j] - data[(i-1)&MASK_X][j];
         float dy = data[i][j] - data[i][(j-1)&MASK_X];
         float slope = dx*dx + dy*dy;
         float slope_color = stb_linear_remap(slope, 0, 1.6, 0,1);
         float t;
         int r,g,b,a;
         height_color = smoothstep(height_color);
         slope_color  = smoothstep(slope_color);

         // vector = (dx, dy, 1) renormalized
         // dot    = (-3/5, 0, 4/5) * normalized vector
         // normalize = 1.0/sqrt(dx*dx+dy*dy+1)
         // dot = (-3/5 * dx + 4/5) * normalize 
         brightness = (0.6*dx + 0.8) / sqrt(slope+1);

         brightness = stb_clamp(brightness, 0,1);
         brightness = stb_lerp(brightness, 0.3, 1.0);
         brightness = stb_clamp(brightness, 0,1);

         if (height[i][j] == 0) {
            height_color = stb_linear_remap(data[i][j], -0.5,2.5, 1,0);
            height_color = stb_clamp(height_color, 0,1);
            r = stb_lerp(height_color, 144, 130);
            g = stb_lerp(height_color, 160, 170);
            b = stb_lerp(height_color, 104, 200);
            brightness = 1;
         } else {
            r = stb_lerp(height_color, 144, 220);
            g = stb_lerp(height_color, 160, 220);
            b = stb_lerp(height_color, 104, 220);

            r = stb_lerp(slope_color,    r,  120);
            g = stb_lerp(slope_color,    g,  120);
            b = stb_lerp(slope_color,    b,  120);

            // grass patches: vary the grass between darker and lighter
            // tones, so the ground is not a single uniform colour
            {
               float patch = stb_perlin_noise3(i / 18.0f, j / 18.0f, 0.37f, 0, 0, 0);
               float detail = stb_perlin_noise3(i / 5.0f, j / 5.0f, 0.71f, 0, 0, 0);
               float v = (patch + detail * 0.35f);
               v = stb_clamp(v, -1.0f, 1.0f);
               v = (v + 1.0f) / 2.0f;            // 0..1
               v = stb_lerp(v, 0.78f, 1.22f);    // darker .. lighter
               r = (int)(r * v);
               g = (int)(g * v);
               b = (int)(b * v);
            }
         }

         r *= brightness;
         g *= brightness;
         b *= brightness;
         a = 0x80;

         c = (r << 16) + (g << 8) + b + (a << 24);
         for (k=0; k < MAP_Z; ++k)
            color[i][j][k] = c;
      }
   }
}

void compute_water_height(int water_squares_desired)
{
   int i,j;
   float wmin, wmax;
   float water_height = 0;
   /* the target and the tolerance are counts of squares: both grow with the
      area, or the bisection cannot land inside 32 of a 4x bigger target and
      the map ends up with no water at all (the 512 map's 32 is 6.25% of the
      region it counts) */
   int tolerance = 32 * ((MAP_X * MAP_Y) / (512 * 512));
   // determine height to place water; we make it so that the number
   // of squares in the blue start region is controlled by the target
   wmin = -32;
   wmax = 32;
   while (wmin < wmax - 1.0/32) {
      int water = 0;
      water_height = (wmin + wmax) / 2;
      /* count the water in the eastern start region (the blue/green marker
         rectangle of build_debug), as a fraction of the map */
      for (i=MAP_X*3/4; i < MAP_X; ++i) {
         for (j=MAP_Y/4; j < MAP_Y*3/4; ++j)
            if (data[i][j] <= water_height)
               ++water;
      }

      if (water > water_squares_desired + tolerance)
         // too much water
         wmax = water_height;
      else if (water < water_squares_desired - tolerance)
         // not enough water
         wmin = water_height;
      else
         break;
   }

   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         data[i][j] = data[i][j] - water_height + 0.99;
      }
   }
}

void build_debug(void)
{
   if (debug) {
      int i,k;
      /* the start markers: blue west, green east, the two regions a .toml
         names as the teams' spawn rectangles */
      for (k=0; k < MAP_Z; ++k) {
         for (i=0; i < MAP_Y/2; ++i) {
            color[MAP_X/4][i+MAP_Y/4][k] = 0x806060ff;
            color[MAP_X*3/4][i+MAP_Y/4][k] = 0x8030d030;
         }
         for (i=MAP_X/32; i < MAP_X/4; ++i) {
            color[i][MAP_Y/4][k] = 0x806060ff;
            color[i][MAP_Y*3/4][k] = 0x806060ff;
            color[MAP_X-1-i][MAP_Y/4][k] = 0x8030d030;
            color[MAP_X-1-i][MAP_Y*3/4][k] = 0x8030d030;
         }
      }
   }
}

int is_surface(int x, int y, int z)
{
   if (map[x][y][z]==0) return 0;
   if (x   >     0 && map[x-1][y][z]==0) return 1;
   if (x+1 < MAP_X && map[x+1][y][z]==0) return 1;
   if (y   >     0 && map[x][y-1][z]==0) return 1;
   if (y+1 < MAP_Y && map[x][y+1][z]==0) return 1;
   if (z   >     0 && map[x][y][z-1]==0) return 1;
   if (z+1 < MAP_Z && map[x][y][z+1]==0) return 1;
   return 0;
}

void write_color(FILE *f, uint32 color)
{
   // assume color is ARGB native, but endianness is unknown

   // file format endianness is ARGB little endian, i.e. B,G,R,A
   fputc((uint8) (color >>  0), f);
   fputc((uint8) (color >>  8), f);
   fputc((uint8) (color >> 16), f);
   fputc((uint8) (color >> 24), f);
}

void write_map(char *filename)
{
   int i,j,k;
   FILE *f = fopen(filename, "wb");

   // first invert map vertically since voxel format has z=0 at top
   for (i=0; i < MAP_X; ++i) {
      for (j=0; j < MAP_Y; ++j) {
         for (k=0; k < MAP_Z>>1; ++k) {
            int z = (MAP_Z-1) - k;
            uint32 t = color[i][j][k];
            color[i][j][k] = color[i][j][z];
            color[i][j][z] = t;
            t = map[i][j][k];
            map[i][j][k] = map[i][j][z];
            map[i][j][z] = t;
         }
      }
   }

   for (j=0; j < MAP_Y; ++j) {
      for (i=0; i < MAP_X; ++i) {
         int written_colors = 0;
         int backpatch_address = -1;
         int previous_bottom_colors = 0;
         int current_bottom_colors = 0;
         int middle_start = 0;

         k = 0;
         while (k < MAP_Z) {
            int z;

            int middle_start;
            int top_colors_start;
            int top_colors_end;
            int bottom_colors_start;
            int bottom_colors_end;
            int top_colors_len;
            int bottom_colors_len;
            int colors;

            // find the "middle" region
            middle_start = k;
            while (k < MAP_Z && !map[i][j][k])
               ++k;
            // find the top region
            top_colors_start = k;
            while (k < MAP_Z && map[i][j][k] && is_surface(i,j,k))
               ++k;
            top_colors_end = k;
            // find the start of the next bottom region
            while (k < MAP_Z && map[i][j][k] && !is_surface(i,j,k))
               ++k;
            // find the bottom colors of the next span
            bottom_colors_start = k;
            // as a special case, check if the bottom colors are actually top colors
            z = k;
            while (z < MAP_Z && is_surface(i,j,z))
               ++z;
            if (z == MAP_Z || 0)
               ; // in this case, the bottom colors of the next span are empty
            else {
               // otherwise, skip past the bottom colors so we can write them as top colors
               while (is_surface(i,j,k))  
                  ++k;
            }
            bottom_colors_end = k;

            // now we're ready to write a span
            top_colors_len = top_colors_end - top_colors_start;
            bottom_colors_len = bottom_colors_end - bottom_colors_start;

            colors = top_colors_len + bottom_colors_len;

            if (k == MAP_Z)
               fputc(0,f); // last span
            else
               fputc(colors+1, f);
            fputc(top_colors_start, f);
            fputc(top_colors_end-1, f);
            fputc(middle_start, f);

            for (z=0; z < top_colors_len; ++z)
               write_color(f, color[i][j][top_colors_start + z]);
            for (z=0; z < bottom_colors_len; ++z)
               write_color(f, color[i][j][bottom_colors_start + z]);
         }  
      }
   }
   fclose(f);
}

void build_map(char **cmdline_options);

int main(int argc, char **argv)
{
   char *opts[256] = { 0 };
   char *output_file = "stbmap.vxl";
   uint32 seed = time(NULL);
   int i;
   char **options = stb_getopt_param(&argc, argv, "so");

   //test_map("c:/program files/ace of spades/vxl/foo.vxl");

   for (i=0; options[i]; ++i) {
      char *opt = options[i];
      switch (opt[0]) {
         case '?':
         case 'h':
            printf("Usage: %s {options}\n"
                   "Options:\n"
                   "   -s {seed}      Random seed; always generates the same map for a seed\n"
                   "   -o {filename}  Filename to write to\n"
                   "   -d             Debug; draws green & blue start regions\n"
                   "   -n             No trees (aos_forest_river: the bare valley)\n"
                   "\n"
                   "If no seed is specified, a random one is generated.\n"
                   "If no filename is written, writes to 'stbmap.vxl'\n");
            return 0;

         case 's': seed = atoi(opt+1); break;
         case 'o': output_file = opt+1; break;
         case 'd': debug = 1; break;
         default:  opts[opt[0]] = opt+1; break;
      }
   }
   printf("Generating to '%s' using seed '%d'\n", output_file, seed);
   stb_srand(seed);

   build_map(opts);
   write_map(output_file);
   printf("Finished\n");
   return 0;
}



// stb_perlin.h -- perlin noise -- v0.1
#include <math.h> // floor()

// not same permutation table as Perlin's reference to avoid copyright issues;
// Perlin's table can be found at http://mrl.nyu.edu/~perlin/noise/
// @OPTIMIZE: should this be unsigned char instead of int for cache?
static int stb__perlin_randtab[512] =
{
   23, 125, 161, 52, 103, 117, 70, 37, 247, 101, 203, 169, 124, 126, 44, 123, 
   152, 238, 145, 45, 171, 114, 253, 10, 192, 136, 4, 157, 249, 30, 35, 72, 
   175, 63, 77, 90, 181, 16, 96, 111, 133, 104, 75, 162, 93, 56, 66, 240, 
   8, 50, 84, 229, 49, 210, 173, 239, 141, 1, 87, 18, 2, 198, 143, 57, 
   225, 160, 58, 217, 168, 206, 245, 204, 199, 6, 73, 60, 20, 230, 211, 233, 
   94, 200, 88, 9, 74, 155, 33, 15, 219, 130, 226, 202, 83, 236, 42, 172, 
   165, 218, 55, 222, 46, 107, 98, 154, 109, 67, 196, 178, 127, 158, 13, 243, 
   65, 79, 166, 248, 25, 224, 115, 80, 68, 51, 184, 128, 232, 208, 151, 122, 
   26, 212, 105, 43, 179, 213, 235, 148, 146, 89, 14, 195, 28, 78, 112, 76, 
   250, 47, 24, 251, 140, 108, 186, 190, 228, 170, 183, 139, 39, 188, 244, 246, 
   132, 48, 119, 144, 180, 138, 134, 193, 82, 182, 120, 121, 86, 220, 209, 3, 
   91, 241, 149, 85, 205, 150, 113, 216, 31, 100, 41, 164, 177, 214, 153, 231, 
   38, 71, 185, 174, 97, 201, 29, 95, 7, 92, 54, 254, 191, 118, 34, 221, 
   131, 11, 163, 99, 234, 81, 227, 147, 156, 176, 17, 142, 69, 12, 110, 62, 
   27, 255, 0, 194, 59, 116, 242, 252, 19, 21, 187, 53, 207, 129, 64, 135, 
   61, 40, 167, 237, 102, 223, 106, 159, 197, 189, 215, 137, 36, 32, 22, 5,  

   // and a second copy so we don't need an extra mask or static initializer
   23, 125, 161, 52, 103, 117, 70, 37, 247, 101, 203, 169, 124, 126, 44, 123, 
   152, 238, 145, 45, 171, 114, 253, 10, 192, 136, 4, 157, 249, 30, 35, 72, 
   175, 63, 77, 90, 181, 16, 96, 111, 133, 104, 75, 162, 93, 56, 66, 240, 
   8, 50, 84, 229, 49, 210, 173, 239, 141, 1, 87, 18, 2, 198, 143, 57, 
   225, 160, 58, 217, 168, 206, 245, 204, 199, 6, 73, 60, 20, 230, 211, 233, 
   94, 200, 88, 9, 74, 155, 33, 15, 219, 130, 226, 202, 83, 236, 42, 172, 
   165, 218, 55, 222, 46, 107, 98, 154, 109, 67, 196, 178, 127, 158, 13, 243, 
   65, 79, 166, 248, 25, 224, 115, 80, 68, 51, 184, 128, 232, 208, 151, 122, 
   26, 212, 105, 43, 179, 213, 235, 148, 146, 89, 14, 195, 28, 78, 112, 76, 
   250, 47, 24, 251, 140, 108, 186, 190, 228, 170, 183, 139, 39, 188, 244, 246, 
   132, 48, 119, 144, 180, 138, 134, 193, 82, 182, 120, 121, 86, 220, 209, 3, 
   91, 241, 149, 85, 205, 150, 113, 216, 31, 100, 41, 164, 177, 214, 153, 231, 
   38, 71, 185, 174, 97, 201, 29, 95, 7, 92, 54, 254, 191, 118, 34, 221, 
   131, 11, 163, 99, 234, 81, 227, 147, 156, 176, 17, 142, 69, 12, 110, 62, 
   27, 255, 0, 194, 59, 116, 242, 252, 19, 21, 187, 53, 207, 129, 64, 135, 
   61, 40, 167, 237, 102, 223, 106, 159, 197, 189, 215, 137, 36, 32, 22, 5,  
};

static float stb__perlin_lerp(float a, float b, float t)
{
   return a + (b-a) * t;
}

// different grad function from Perlin's, but easy to modify to match reference
static float stb__perlin_grad(int hash, float x, float y, float z)
{
   static float basis[12][4] =
   {
      {  1, 1, 0 },
      { -1, 1, 0 },
      {  1,-1, 0 },
      { -1,-1, 0 },
      {  1, 0, 1 },
      { -1, 0, 1 },
      {  1, 0,-1 },
      { -1, 0,-1 },
      {  0, 1, 1 },
      {  0,-1, 1 },
      {  0, 1,-1 },
      {  0,-1,-1 },
   };

   // perlin's gradient has 12 cases so some get used 1/16th of the time
   // and some 2/16ths. We reduce bias by changing those fractions
   // to 5/16ths and 6/16ths, and the same 4 cases get the extra weight.
   static unsigned char indices[64] =
   {
      0,1,2,3,4,5,6,7,8,9,10,11,
      0,9,1,11,
      0,1,2,3,4,5,6,7,8,9,10,11,
      0,1,2,3,4,5,6,7,8,9,10,11,
      0,1,2,3,4,5,6,7,8,9,10,11,
      0,1,2,3,4,5,6,7,8,9,10,11,
   };

   // if you use reference permutation table, change 63 below to 15 to match reference
   float *grad = basis[indices[hash & 63]];
   return grad[0]*x + grad[1]*y + grad[2]*z;
}

float stb_perlin_noise3(float x, float y, float z, int x_wrap, int y_wrap, int z_wrap)
{
   float u,v,w;
   float n000,n001,n010,n011,n100,n101,n110,n111;
   float n00,n01,n10,n11;
   float n0,n1;

   unsigned int x_mask = x_wrap < 256 ? x_wrap-1 : 255;
   unsigned int y_mask = y_wrap < 256 ? y_wrap-1 : 255;
   unsigned int z_mask = z_wrap < 256 ? z_wrap-1 : 255;
   int px = (int) floor(x);
   int py = (int) floor(y);
   int pz = (int) floor(z);
   int x0 = px & x_mask, x1 = (px+1) & x_mask;
   int y0 = py & y_mask, y1 = (py+1) & y_mask;
   int z0 = pz & z_mask, z1 = (pz+1) & z_mask;
   int r0,r1, r00,r01,r10,r11;

   #define stb__perlin_ease(a)   (((a*6-15)*a + 10) * a * a * a)

   x -= px; u = stb__perlin_ease(x);
   y -= py; v = stb__perlin_ease(y);
   z -= pz; w = stb__perlin_ease(z);

   r0 = stb__perlin_randtab[x0];
   r1 = stb__perlin_randtab[x1];

   r00 = stb__perlin_randtab[r0+y0];
   r01 = stb__perlin_randtab[r0+y1];
   r10 = stb__perlin_randtab[r1+y0];
   r11 = stb__perlin_randtab[r1+y1];

   n000 = stb__perlin_grad(stb__perlin_randtab[r00+z0], x  , y  , z   );
   n001 = stb__perlin_grad(stb__perlin_randtab[r00+z1], x  , y  , z-1 );
   n010 = stb__perlin_grad(stb__perlin_randtab[r01+z0], x  , y-1, z   );
   n011 = stb__perlin_grad(stb__perlin_randtab[r01+z1], x  , y-1, z-1 );
   n100 = stb__perlin_grad(stb__perlin_randtab[r10+z0], x-1, y  , z   );
   n101 = stb__perlin_grad(stb__perlin_randtab[r10+z1], x-1, y  , z-1 );
   n110 = stb__perlin_grad(stb__perlin_randtab[r11+z0], x-1, y-1, z   );
   n111 = stb__perlin_grad(stb__perlin_randtab[r11+z1], x-1, y-1, z-1 );

   n00 = stb__perlin_lerp(n000,n100,u);
   n01 = stb__perlin_lerp(n001,n101,u);
   n10 = stb__perlin_lerp(n010,n110,u);
   n11 = stb__perlin_lerp(n011,n111,u);

   n0 = stb__perlin_lerp(n00,n10,v);
   n1 = stb__perlin_lerp(n01,n11,v);

   return stb__perlin_lerp(n0,n1,w);
}

