#include <vitasdk.h>
#include <vitaGL.h>
#include "renderer.h"

#include "shaders/simple_shader_f.h"
#include "shaders/simple_shader_v.h"
#include "shaders/bubble_shader_v.h"
#include "shaders/bubble_shader_f.h"

static float shader_pos[8] = {
	-1.0f, 1.0f,
	-1.0f, -1.0f,
	 1.0f, 1.0f,
	 1.0f, -1.0f
};

static float shader_texcoord[8] = {
	0.0f, 1.0f,
	0.0f, 0.0f,
	1.0f, 1.0f,
	1.0f, 0.0f
};

static GLuint bubble_fbo;
static GLuint bubble_fbo_tex;
static GLuint bubble_prog;
static GLuint simple_prog;
static GLint bubble_time_unif;

void prepare_simple_drawer() {
	GLuint vshad = glCreateShader(GL_VERTEX_SHADER);
	GLuint fshad = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderBinary(1, &vshad, 0, shader_v, size_shader_v);
	glShaderBinary(1, &fshad, 0, shader_f, size_shader_f);

	simple_prog = glCreateProgram();
	glAttachShader(simple_prog, vshad);
	glAttachShader(simple_prog, fshad);
	glBindAttribLocation(simple_prog, 0, "inPos");
	glBindAttribLocation(simple_prog, 1, "inTex");
	glLinkProgram(simple_prog);
	glUniform1i(glGetUniformLocation(simple_prog, "tex"), 0);
	glDeleteShader(vshad);
	glDeleteShader(fshad);
}

void prepare_bubble_drawer() {
	GLuint vshad = glCreateShader(GL_VERTEX_SHADER);
	GLuint fshad = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderBinary(1, &vshad, 0, bubble_shader_v, size_bubble_shader_v);
	glShaderBinary(1, &fshad, 0, bubble_shader_f, size_bubble_shader_f);
	
	bubble_prog = glCreateProgram();
	glAttachShader(bubble_prog, vshad);
	glAttachShader(bubble_prog, fshad);
	glBindAttribLocation(bubble_prog, 0, "aPos");
	glBindAttribLocation(bubble_prog, 1, "aTex");
	glLinkProgram(bubble_prog);
	glUniform1i(glGetUniformLocation(bubble_prog, "u_texture"), 0);
	bubble_time_unif = glGetUniformLocation(bubble_prog, "u_time");
	glDeleteShader(vshad);
	glDeleteShader(fshad);
	
	glGenFramebuffers(1, &bubble_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, bubble_fbo);
	glGenTextures(1, &bubble_fbo_tex);
	glTextureImage2D(bubble_fbo_tex, GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, bubble_fbo_tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void draw_simple_texture(GLuint tex) {
	glBindTexture(GL_TEXTURE_2D, tex);
	glUseProgram(simple_prog);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, &shader_pos[0]);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, &shader_texcoord[0]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(0);
}

GLuint draw_bubble_icon(GLuint tex) {
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, bubble_fbo);
	glViewport(0, 0, 128, 128);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(bubble_prog);
	float time = (float)(sceKernelGetProcessTimeLow()) / 1000000.0f;
	glBindTexture(GL_TEXTURE_2D, tex);
	glUniform1f(bubble_time_unif, time);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), shader_pos);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), shader_texcoord);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, 960, 544);
	glEnable(GL_SCISSOR_TEST);
	return bubble_fbo_tex;
}
