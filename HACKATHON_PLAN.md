# Execution Plan

## Phase 1: Python Orchestration Layer
1. Scaffold a FastAPI server in `backend/main.py`.
2. Create a POST route `/compile` that accepts a JSON payload containing a `base64_image` and a `user_prompt`.
3. Integrate the Cerebras API call using the OpenAI Python SDK. Set the model to `gemma-4-31b`.
4. Configure the API call to accept the Base64 image in the `messages` array and enforce a strict JSON output schema containing a single key: `glsl_fragment`.
5. Add the `reasoning_effort="high"` parameter to the API request.

## Phase 2: C++ Renderer Scaffolding
1. Create a `CMakeLists.txt` file configured for C++17, linking OpenGL, GLFW, GLEW, and threading.
2. Download header-only libraries: `stb_image.h`, `stb_image_write.h`, and `httplib.h` into a `vendor/` directory.
3. Write `src/main.cpp` to initialize a basic 800x600 GLFW window and render a screen-spanning quad.
4. Implement a utility function to load, compile, and link a basic passthrough vertex and fragment shader.

## Phase 3: The JIT Integration Loop
1. Add a frame-capture function in C++ using `glReadPixels` and `stbi_write_png_to_func` to encode the current framebuffer directly to a Base64 string in memory.
2. Implement an asynchronous HTTP POST request using `httplib` that sends the Base64 frame to the local Python FastAPI server.
3. Parse the returned JSON payload in C++, extract the `glsl_fragment` string, recompile the OpenGL shader program on the fly, and swap it into the active render state.
4. Bind this capture-and-recompile loop to a keyboard press (e.g., the 'Spacebar') so the user can trigger the agent loop manually during the demo.