import json
import os

import openai
from fastapi import FastAPI, HTTPException
from fastapi.responses import RedirectResponse
from pydantic import BaseModel, Field

app = FastAPI(title="Shader JIT Orchestrator")

_MODEL = "gemma-4-31b"
_BASE_URL = "https://api.cerebras.ai/v1"

_SYSTEM_PROMPT = (
    "You are a GLSL shader expert. Given a rendered frame and a user prompt, "
    "generate a complete GLSL 3.3 core fragment shader. "
    "Requirements: declare 'in vec2 vUV;', 'out vec4 FragColor;', and optionally "
    "'uniform float uTime;'. Return ONLY the complete shader source as the value of 'glsl_fragment'."
)

_OUTPUT_SCHEMA = {
    "type": "json_schema",
    "json_schema": {
        "name": "shader_output",
        "strict": True,
        "schema": {
            "type": "object",
            "properties": {
                "glsl_fragment": {"type": "string"},
            },
            "required": ["glsl_fragment"],
            "additionalProperties": False,
        },
    },
}


@app.get("/", include_in_schema=False)
async def root() -> RedirectResponse:
    return RedirectResponse(url="/docs")


# 1x1 transparent PNG — valid placeholder for Swagger UI testing
_EXAMPLE_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ"
    "AAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
)


class CompileRequest(BaseModel):
    base64_image: str = Field(examples=[_EXAMPLE_PNG])
    user_prompt: str = Field(examples=["make it pulse red and blue"])


class CompileResponse(BaseModel):
    glsl_fragment: str


@app.post("/compile", response_model=CompileResponse)
async def compile_shader(req: CompileRequest) -> CompileResponse:
    api_key = os.environ.get("CEREBRAS_API_KEY")
    if not api_key:
        raise HTTPException(status_code=503, detail="CEREBRAS_API_KEY not set")

    client = openai.AsyncOpenAI(api_key=api_key, base_url=_BASE_URL)

    try:
        response = await client.chat.completions.create(
            model=_MODEL,
            messages=[
                {"role": "system", "content": _SYSTEM_PROMPT},
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": f"data:image/png;base64,{req.base64_image}",
                            },
                        },
                        {"type": "text", "text": req.user_prompt},
                    ],
                },
            ],
            response_format=_OUTPUT_SCHEMA,  # type: ignore[arg-type]
            max_completion_tokens=32768,
            temperature=0.2,
            top_p=1,
        )
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    content = response.choices[0].message.content
    if not content:
        raise HTTPException(status_code=502, detail="Empty response from model")
    try:
        data = json.loads(content)
        return CompileResponse(glsl_fragment=data["glsl_fragment"])
    except (json.JSONDecodeError, KeyError) as exc:
        raise HTTPException(
            status_code=502, detail=f"Bad model output: {content}"
        ) from exc
