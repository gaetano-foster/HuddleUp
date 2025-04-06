#include <stdio.h>
#include <SDL.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <Windows.h>
#include <gl/GL.h>

#define SCR_W			800
#define SCR_H			600
#define ASPECT_RATIO	1.33f

// data types simply for ease of typing 
typedef uint8_t byte_t;
typedef uint16_t word_t;
typedef uint32_t dword_t;
typedef uint64_t qword_t;
typedef uint32_t bool_t;

typedef struct object_t
{
	float x; // x position
	float y; // y position
	float a; // angle in radians
	// float z
} object_t;

dword_t keydown[SDL_NUM_SCANCODES]; // for input

typedef struct gameloop_t
{
	bool_t running;
	clock_t last_time;			// time since last iteration of loop
	clock_t time;				// total time since the loop started
	clock_t timer;
	int32_t frames_per_second;
	double ns;					// time per tick
	double delta; 
	double delta_time;
	int32_t frames;				// frames elapsed since we last displayed fps
} gameloop_t;

typedef struct plane_t {
	SDL_Surface *tex;
	SDL_Color *raw_tex; // raw pixel data; just colors
	int32_t tex_width;
	int32_t tex_height;
	// each plane has its own local "near" and "far" plane distances because the camera should render different planes differently
	float local_near; // near plane for how the camera will render the plane
	float local_far; // far plane
	// should this be rendered as a floor or ceiling plane?
	bool_t orientation; // 1 = ceiling, 2 = floor
	//TODO: possibly implement a way to render at different locations?
} plane_t;

// will store the state of the app, including ceiling, floor, objects, etc.
// for now just the player, ceiling texture and floor texture
typedef struct state_t 
{
	gameloop_t gameloop;
	object_t camera; // will be an array with player at index 0 eventually
	plane_t floor;
	plane_t ceiling;
} state_t;

// wrapper around sdl window and renderer for window management
typedef struct display_t
{
	SDL_Window *win;
	SDL_GLContext context;
} display_t;

SDL_Color
GetPixelFromSurface(
	SDL_Surface *surface,
	int32_t x,
	int32_t y
) {
	int bpp = surface->format->BytesPerPixel;
	byte_t *p = (byte_t*)surface->pixels + y * surface->pitch + x * bpp;
	dword_t data;

	switch (bpp)
	{
	case 1:
		data = *p;
		break;

	case 2:
		data = *(word_t*)p;
		break;

	case 3:
		if (SDL_BYTEORDER == SDL_BIG_ENDIAN)
			data = p[0] << 16 | p[1] << 8 | p[2];
		else
			data = p[0] | p[1] << 8 | p[2] << 16;
		break;

	case 4:
		data = *(dword_t*)p;
		break;

	default:
		data = 0;
	}

	SDL_Color rgb;
	SDL_GetRGB(data, surface->format, &rgb.r, &rgb.g, &rgb.b);
	return rgb;
}

bool_t
ConstructDisplay(
	display_t *display,
	const char *title,
	dword_t width,
	dword_t height
) {
	if (!(display->win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_OPENGL))) {
		fprintf(stderr, "Couldn't initialize display.\n%s", SDL_GetError());
		return 0;
	}

	if (!(display->context = SDL_GL_CreateContext(display->win))) {
		fprintf(stderr, "Couldn't initialize OpenGL.\n%s", SDL_GetError());
	}

	return 1;
}

void
DestroyDisplay(
	display_t *display
) {
	SDL_GL_DeleteContext(display->context);
	SDL_DestroyWindow(display->win);
}

bool_t
CreatePlane(
	plane_t *plane,
	float near_dist,
	float far_dist,
	bool_t orientation,
	const char *tex_path
) {
	plane->local_near = near_dist;
	plane->local_far = far_dist;
	plane->orientation = orientation;
	if (!(plane->tex = SDL_LoadBMP(tex_path))) {
		fprintf(stderr, "Error: Couldn't load texture from %s.\n%s\n", tex_path, SDL_GetError());
		return 0;
	}

	plane->tex_height = plane->tex->h;
	plane->tex_width = plane->tex->w;
	if (!(plane->raw_tex = (SDL_Color*)malloc(sizeof(SDL_Color) * plane->tex_height * plane->tex_width))) {
		fprintf(stderr, "Error: Couldn't allocate memory for texture at %s.\n", tex_path);
		return 0;
	}
	for (int y = 0; y < plane->tex_height; y++) {
		for (int x = 0; x < plane->tex_width; x++) {
			plane->raw_tex[y * plane->tex_width + x] = GetPixelFromSurface(plane->tex, x, y);
		}
	}

	SDL_FreeSurface(plane->tex);
	
	return 1;
}

void
DestroyPlane(
	plane_t *plane
) {
	free(plane->raw_tex);
}

bool_t
RenderPlane(
	//SDL_Renderer *ren,
	plane_t plane,
	float cx,	// camera x coord
	float cy,	// camera y coord
	float ca,	// camera angle in radians
	float fov	// camera field of view in radians 
) {
	// positions of start and end points on near and far plane
	float far_1_x = (cx + cosf(ca - fov/2) * plane.local_far);
	float far_1_y = (cy + sinf(ca - fov/2) * plane.local_far);

	float near_1_x = (cx + cosf(ca - fov/2) * plane.local_near);
	float near_1_y = (cy + sinf(ca - fov/2) * plane.local_near);

	float far_2_x = (cx + cosf(ca + fov/2) * plane.local_far);
	float far_2_y = (cy + sinf(ca + fov/2) * plane.local_far);

	float near_2_x = (cx + cosf(ca + fov/2) * plane.local_near);
	float near_2_y = (cy + sinf(ca + fov/2) * plane.local_near);


	int corner_1_x = 0;
	int corner_1_y = 0;

	int corner_2_x = 0;
	int corner_2_y = 0;

	int corner_3_x = 0;
	int corner_3_y = 0;

	int corner_4_x = 0;
	int corner_4_y = 0;

	for (int y = 1; y < SCR_H / 2; y++) {
		float sample_depth = (float)y / ((float)SCR_H / 2.0f); // y = depth; closer to vanishing point should be smaller

		float start_x = (far_1_x - near_1_x) / sample_depth + near_1_x; // ray start position
		float start_y = (far_1_y - near_1_y) / sample_depth + near_1_y;

		float   end_x = (far_2_x - near_2_x) / sample_depth + near_2_x;  // ray end position
		float   end_y = (far_2_y - near_2_y) / sample_depth + near_2_y;

		for (int x = 1; x < SCR_W; x++) {
			float sample_width = (float)x / (float)SCR_W;

			float sample_x = (end_x - start_x) * sample_width + start_x;
			float sample_y = (end_y - start_y) * sample_width + start_y;

			SDL_Color rgb;

			int tx = (int)(sample_x * (plane.tex_width));
			int ty = (int)(sample_y * (plane.tex_height));
			int dy = plane.orientation ? y + SCR_H / 2 : SCR_H - (y + SCR_H / 2); // determine pixel y position based on plane orientation and distance from camera

			if (isnan(tx) || isnan(ty)) {
				rgb = (SDL_Color) { 0, 0, 0 };
			}
			else if (tx < 0 || ty < 0) {
				rgb = (SDL_Color) { 0, 0, 0 };
			}
			else if (tx > plane.tex_width || ty > plane.tex_height) {
				rgb = (SDL_Color) { 0, 0, 0 };
			}
			else {
				rgb = plane.raw_tex[ty * plane.tex_width + tx];
				SDL_Color red = (SDL_Color) { 255, 0, 0 };

				if (tx == 0 && ty == 0) {
					corner_1_x = x;
					corner_1_y = dy;
				}
				else if (tx == plane.tex_width && ty == 0) {
					corner_2_x = x;
					corner_2_y = dy;
				}
				else if (tx == 0 && ty == plane.tex_height) {
					corner_3_x = x;
					corner_3_y = dy;
				}
				else if (tx == plane.tex_width && ty == plane.tex_height) {
					corner_4_x = x;
					corner_4_y = dy;
				}
			}
			// TODO: Implement real projection and gpu rendering
			glColor3ub(rgb.r, rgb.g, rgb.b); 
			glPointSize(1.0f);
			glBegin(GL_POINTS);
			glVertex2i(x, dy);
			glEnd();
		}
	}
}

bool_t
InitGameLoop(
	state_t *state,
	int32_t fps
) {
	state->gameloop.running = 1;
	state->gameloop.last_time = clock();
	state->gameloop.time = 0;
	state->gameloop.timer = 0;
	state->gameloop.frames_per_second = fps;
	state->gameloop.ns	= CLOCKS_PER_SEC / state->gameloop.frames_per_second;
	state->gameloop.delta = 0; 
	state->gameloop.delta_time = 1 / state->gameloop.frames_per_second;
	state->gameloop.frames = 0;
	return 1;
}

void
InitCamera(
	state_t *state
) {
	state->camera.x = 0;
	state->camera.y = 0;
	state->camera.a = 0.1f;
	return 1;
}

bool_t
Init(
	state_t *state
) {
	SDL_Init(SDL_INIT_EVERYTHING);
	InitCamera(state);
	InitGameLoop(state, 72);
	// opengl init
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, SCR_W, SCR_H, 0, -1, 1); // left, right, bottom, top, near, far
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	if (!CreatePlane(&(state->floor), 0.001f, 0.06f, 1, "res/gfx/floor.bmp")) {
		return 0;
	}
	//state->ceiling.tex = SDL_LoadBMP("res/gfx/sky.bmp");

	return 1;
}

bool_t
DestroyState(
	state_t *state
) {
	DestroyPlane(&(state->floor));
}

bool_t
Tick(
	state_t *state
) {

	if (keydown[SDL_SCANCODE_W]) {
		state->camera.x += cosf(state->camera.a) * state->gameloop.delta_time * 0.01f;
		state->camera.y += sinf(state->camera.a) * state->gameloop.delta_time * 0.01f;
	}											 							  
	if (keydown[SDL_SCANCODE_S]) {				 							  
		state->camera.x -= cosf(state->camera.a) * state->gameloop.delta_time * 0.01f;
		state->camera.y -= sinf(state->camera.a) * state->gameloop.delta_time * 0.01f;
	}

	if (keydown[SDL_SCANCODE_D]) {
		state->camera.a += state->gameloop.delta_time * 0.1f;
	}

	if (keydown[SDL_SCANCODE_A]) {
		state->camera.a -= state->gameloop.delta_time * 0.1f;
	}

	/*if (keydown[SDL_SCANCODE_UP]) {
		far += 0.001f * state->gameloop.delta_time;
		printf("Far Plane Distance: %f\n", far);
	}

	if (keydown[SDL_SCANCODE_DOWN]) {
		far -= 0.001f * state->gameloop.delta_time;
		printf("Far Plane Distance: %f\n", far);
	}

	if (keydown[SDL_SCANCODE_RIGHT]) {
		near += 0.001f * state->gameloop.delta_time;
		printf("Near Plane Distance: %f\n", near);
	}

	if (keydown[SDL_SCANCODE_LEFT]) {
		near -= 0.001f * state->gameloop.delta_time;
		printf("Near Plane Distance: %f\n", near);
	}*/

	return 1;
}

bool_t
Render(
	display_t *display,
	state_t *state 
) {
	glViewport(0, 0, SCR_W, SCR_H);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	RenderPlane(state->floor, state->camera.x, state->camera.y, state->camera.a, 1.57f);

	SDL_GL_SwapWindow(display->win);
	return 1;
}

bool_t
Run()
{
	state_t state;
	display_t display;  
	ConstructDisplay(&display, "Mode7", SCR_W, SCR_H);
	SDL_Event e;

	if (!Init(&state)) {
		fprintf(stderr, "Error: Couldn't initialize Mode7.");
		DestroyDisplay(&display);
		return 0;
	}

	while (state.gameloop.running) {
		clock_t now = clock();
		state.gameloop.delta += (now - state.gameloop.last_time) / state.gameloop.ns;
		state.gameloop.timer += now - state.gameloop.last_time;
		state.gameloop.last_time = now;

		if (state.gameloop.delta >= 1) {
			while (SDL_PollEvent(&e)) {
				if (e.type == SDL_QUIT) state.gameloop.running = 0;
				if (e.type == SDL_KEYDOWN) keydown[e.key.keysym.scancode] = 1;
				if (e.type == SDL_KEYUP) keydown[e.key.keysym.scancode] = 0;
			}
			if (!Tick(&state)) state.gameloop.running = 0;
			if (!Render(&display, &state)) state.gameloop.running = 0;
			state.gameloop.frames++;
			state.gameloop.delta--;
			if (state.gameloop.timer >= CLOCKS_PER_SEC) {
				printf("%d\n", state.gameloop.frames);
				state.gameloop.time += CLOCKS_PER_SEC;
				state.gameloop.delta_time = (double)(1 / (double)state.gameloop.frames) * 25;
				state.gameloop.timer = 0;
				state.gameloop.frames = 0;
			}
		}
	}
	DestroyDisplay(&display);
	DestroyState(&state);

	return 1;
}

int
main(
	int argc,
	char **argv
) {	
	Run();
	return 0;
}