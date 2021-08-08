#version 330 core
layout (location = 0) in vec2 aVertex;

uniform mat4 uMVPMatrix;

void main()
{
   gl_Position = uMVPMatrix * vec4(aVertex, 0.0f, 1.0f);
}
