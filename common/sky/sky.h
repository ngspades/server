// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	The time of day and the weather of protocol 1.0, the ideas of the
	aosprotocol specification's Daytime and Weather extension
	(extensions/daytime-weather.md, github.com/zerospades/aosprotocol): its
	night of full darkness lit only by flashlights, the day that passes it
	kept a field for, and the weather it kept two bytes for. Shared by the
	server (the bots see by it) and the client (it draws by it), so both
	agree on how dark a minute is and how far a weather lets one see.

	Header only like common/modes: functions, so a file that includes it and
	uses nothing gets no unused-variable warning (the server builds with
	-Werror).

	Wire (GenericCommand, docs/protocol-1.0.md):
	  server -> client  "sky <minutes> <rate> <weather> <intensity> <moonlight>"
	minutes since midnight (0..1440, a decimal), rate the minutes of the day
	that pass in a real second (0: the time stands), weather a name below,
	intensity and moonlight 0..100.
*/

#ifndef SHARED_SKY_H
#define SHARED_SKY_H

#include <math.h>
#include <string.h>

#define GAME_SKY_DAY_MINUTES 1440.0F
#define GAME_SKY_WEATHERS	 5

/* blocks a bot sees an unlit body at in full darkness; moonlight adds to it */
#define GAME_SKY_NIGHT_SIGHT 24.0F
#define GAME_SKY_MOON_SIGHT	 0.6F /* blocks a point of moonlight */

/* in the wire's order: clear, rain, snow, fog, storm (rain with lightning) */
static inline const char* game_sky_weather_name(int w) {
	static const char* const names[GAME_SKY_WEATHERS] = {"clear", "rain", "snow", "fog", "storm"};
	return w >= 0 && w < GAME_SKY_WEATHERS ? names[w] : "clear";
}

static inline int game_sky_weather_find(const char* name) {
	for(int w = 0; w < GAME_SKY_WEATHERS; w++)
		if(!strcmp(name, game_sky_weather_name(w)))
			return w;
	return -1;
}

/* the sun's height, -1 at midnight, 1 at noon */
static inline float game_sky_sun(float minutes) {
	return -cosf(minutes / GAME_SKY_DAY_MINUTES * 6.2831853F);
}

static inline float game_sky_smooth(float a, float b, float x) {
	float t = (x - a) / (b - a);
	t = t < 0.0F ? 0.0F : t > 1.0F ? 1.0F : t;
	return t * t * (3.0F - 2.0F * t);
}

/* 0 at night, 1 by day: full light from ~8:00 to ~16:30, half at ~6:20
   and ~17:40, dark from ~19:00 to ~5:00 */
static inline float game_sky_daylight(float minutes) {
	return game_sky_smooth(-0.3F, 0.2F, game_sky_sun(minutes));
}

/* 0..1: how warm the light is, highest at sunrise and sunset */
static inline float game_sky_dusk(float minutes) {
	float s = game_sky_sun(minutes) + 0.05F;
	float w = 1.0F - fabsf(s) / 0.25F;
	return w < 0.0F ? 0.0F : w;
}

/* how much of the light a weather takes (0..1, intensity 0..1) */
static inline float game_sky_weather_dark(int weather, float intensity) {
	static const float dark[GAME_SKY_WEATHERS] = {0.0F, 0.35F, 0.12F, 0.2F, 0.55F};
	return weather >= 0 && weather < GAME_SKY_WEATHERS ? dark[weather] * intensity : 0.0F;
}

/* the part of the view distance a weather leaves (1 in clear weather) */
static inline float game_sky_visibility(int weather, float intensity) {
	static const float lost[GAME_SKY_WEATHERS] = {0.0F, 0.45F, 0.6F, 0.8F, 0.6F};
	return weather >= 0 && weather < GAME_SKY_WEATHERS ? 1.0F - lost[weather] * intensity : 1.0F;
}

/* how far an unlit body is seen, of the server's fog `fog`: all of it by
   day in clear weather, in the dark only what moonlight shows, and the
   weather's share of that */
static inline float game_sky_sight(float fog, float minutes, int weather, float intensity, float moonlight) {
	float night = GAME_SKY_NIGHT_SIGHT + GAME_SKY_MOON_SIGHT * moonlight;
	if(night > fog)
		night = fog;
	float d = game_sky_daylight(minutes);
	float sight = (night + (fog - night) * d) * game_sky_visibility(weather, intensity);
	return sight < GAME_SKY_NIGHT_SIGHT * 0.5F ? GAME_SKY_NIGHT_SIGHT * 0.5F : sight;
}

#endif
