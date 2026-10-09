/*------------------.
| :: Description :: |
'-------------------/

    Layer (version 1.0)

    Author: CeeJay.dk
    License: MIT

    About:
    Blends an image with the game.
    The idea is to give users with graphics skills the ability to create effects using a layer just like in an image editor.
    Maybe they could use this to create custom CRT effects, custom vignettes, logos, custom hud elements, toggable help screens and crafting tables or something I haven't thought of.

    The image is Layer.png (or LAYER_SOURCE) with the size LAYER_SIZE_X x LAYER_SIZE_Y. It is drawn as its own
    rectangle, so it can be small, moved, rotated and scaled, and pixels outside it cost nothing.

    More layers: copy this file under another name (Layer2.fx, Logo.fx ...) and put the image next to it with the
    same name (Layer2.png, Logo.png ...). Each copy is a layer of its own: own settings, own textures, and its own
    preprocessor definitions when they are set in the effect's own list.

    Place with mouse (DirectX 10 and newer, Vulkan, OpenGL): tick "Place with mouse" and, with the ReShade overlay
    open, drag the layer with the left mouse button, rotate it by dragging with the right one, scale it with the
    mouse wheel; the middle button goes back to the slider values. The values are shown next to the mouse: type
    them into the sliders (effects cannot save them on their own) and untick the box.

    Blend modes: all 27 of BlendModes.fxh. LAYER_BLEND_STATE 1 - 9 lets the GPU's blend stage do one of the
    simple modes instead, so the shader does not read the game image at all (the fastest way):
    1 Normal, 2 Multiply, 3 Screen, 4 Linear Dodge (Add), 5 Darken, 6 Lighten, 7 Linear Burn, 8 Exclusion,
    9 Subtract. Darken and Lighten have no opacity there: a layer pixel counts when alpha * Blend >= 0.5.
    Add, Linear Burn and Subtract apply the opacity before the clamp there (like an image editor's Fill),
    the shader modes after it (like Opacity); at full opacity they are the same.

    Ideas for future improvement:
    * Tiling control
    * A default Layer texture with something useful in it

    History:
    (*) Feature (+) Improvement (x) Bugfix (-) Information (!) Compatibility

    Version 0.2 by seri14 & Marot Satil
    * Added the ability to scale and move the layer around on XY axis

    Version 1.0
    * Blend modes (BlendModes.fxh), and LAYER_BLEND_STATE for the blend stage
    * Rotation and pivot; the layer is drawn as its own rectangle
    * Place with mouse
    * Copies of the file work side by side as separate layers
    ! Layer_Pos, Layer_Scale and Layer_Blend work as before, so old presets look the same
*/

#include "ReShade.fxh"
#include "BlendModes.fxh"

#ifndef LAYER_SOURCE
    #define LAYER_SOURCE __FILE_STEM__ ".png"   // Layer.fx -> Layer.png, Layer2.fx -> Layer2.png
#endif
#ifndef LAYER_SIZE_X
    #define LAYER_SIZE_X 1280
#endif
#ifndef LAYER_SIZE_Y
    #define LAYER_SIZE_Y 720
#endif
#ifndef LAYER_SINGLECHANNEL
    #define LAYER_SINGLECHANNEL 0
#endif
#ifndef LAYER_BLEND_STATE
    #define LAYER_BLEND_STATE 0                 // 0 = blend in the shader (all modes), 1 - 9 = blend stage
#endif
#ifndef LAYER_MOUSE_EDIT
    #define LAYER_MOUSE_EDIT 1                  // 0 removes the mouse placement completely
#endif

#if LAYER_SINGLECHANNEL
    #define TEXFORMAT R8
#else
    #define TEXFORMAT RGBA8
#endif

// Direct3D 9 cannot read a texture in a vertex shader: no mouse placement there.
#define LAYER_MOUSE (LAYER_MOUSE_EDIT && __RENDERER__ >= 0xa000)

// Texture names with the file's hash, so copies of this file do not share them.
#define LAYER_CAT2(a, b) a##b
#define LAYER_CAT(a, b) LAYER_CAT2(a, b)
#define LAYER_NAME(a) LAYER_CAT(a, __FILE_STEM_HASH__)

#include "ReShadeUI.fxh"

uniform float2 Layer_Pos < __UNIFORM_DRAG_FLOAT2
    ui_label = "Layer Position";
    ui_min = 0.0; ui_max = 1.0;
    ui_step = (1.0 / 200.0);
> = float2(0.5, 0.5);

uniform float Layer_Scale < __UNIFORM_DRAG_FLOAT1
    ui_label = "Layer Scale";
    ui_min = (1.0 / 100.0); ui_max = 4.0;
    ui_step = (1.0 / 250.0);
> = 1.0;

uniform float Layer_Angle < __UNIFORM_DRAG_FLOAT1
    ui_label = "Layer Rotation";
    ui_tooltip = "In degrees, clockwise.";
    ui_min = -180.0; ui_max = 180.0;
    ui_step = 0.1;
> = 0.0;

uniform float2 Layer_Pivot < __UNIFORM_DRAG_FLOAT2
    ui_label = "Layer Pivot";
    ui_tooltip = "The point of the layer it rotates and scales around (0.5, 0.5 = its center).";
    ui_min = 0.0; ui_max = 1.0;
    ui_step = (1.0 / 200.0);
> = float2(0.5, 0.5);

#if LAYER_BLEND_STATE == 0
uniform int Layer_Mode <
    ui_type = "combo";
    ui_label = "Blend Mode";
    ui_items = BLENDMODES_LIST;
> = 0;
#endif

uniform float Layer_Blend < __UNIFORM_SLIDER_FLOAT1
    ui_label = "Layer Blend";
    ui_tooltip = "How much to blend layer with the original image.";
    ui_min = 0.0; ui_max = 1.0;
    ui_step = (1.0 / 255.0); // for slider and drag
> = 1.0;

#if LAYER_MOUSE
uniform bool Layer_Mouse <
    ui_label = "Place with mouse";
    ui_tooltip = "With the overlay open: left drag moves, right drag rotates, the wheel scales, middle click resets.\n"
                 "Type the values shown next to the mouse into the sliders, then untick this.";
> = false;

uniform float2 Layer_MousePoint < source = "mousepoint"; >;
uniform float2 Layer_MouseDelta < source = "mousedelta"; >;
uniform float2 Layer_MouseWheel < source = "mousewheel"; min = -10000.0; max = 10000.0; step = 1.0; >;
uniform bool Layer_LeftButton < source = "mousebutton"; keycode = 0; mode = ""; >;
uniform bool Layer_RightButton < source = "mousebutton"; keycode = 1; mode = ""; >;
uniform bool Layer_MiddleClick < source = "mousebutton"; keycode = 2; mode = "press"; >;
uniform bool Layer_OverlayOpen < source = "overlay_open"; >;
uniform bool Layer_OverlayHovered < source = "overlay_hovered"; >;
#endif

texture LAYER_NAME(Layer_Tex_) <
    source = LAYER_SOURCE;
> {
    Format = TEXFORMAT;
    Width  = LAYER_SIZE_X;
    Height = LAYER_SIZE_Y;
};

sampler LAYER_NAME(Layer_Sampler_)
{
    Texture  = LAYER_NAME(Layer_Tex_);
    AddressU = BORDER;
    AddressV = BORDER;
};

#if LAYER_MOUSE
// Placement while "Place with mouse" is on: texel 0 = position, rotation, log2(scale); texel 1 = the slider
// values it was taken from (when they change, the sliders win).
texture LAYER_NAME(Layer_State_) { Width = 2; Height = 1; Format = RGBA32F; };
texture LAYER_NAME(Layer_StatePrev_) { Width = 2; Height = 1; Format = RGBA32F; };
sampler LAYER_NAME(Layer_StateSampler_) { Texture = LAYER_NAME(Layer_State_); MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
sampler LAYER_NAME(Layer_StatePrevSampler_) { Texture = LAYER_NAME(Layer_StatePrev_); MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
#endif

// Position, rotation (degrees) and scale in effect: the sliders, or the mouse placement.
void Layer_Placement(out float2 pos, out float angle, out float scale)
{
    pos = Layer_Pos;
    angle = Layer_Angle;
    scale = Layer_Scale;
#if LAYER_MOUSE
    if (Layer_Mouse)
    {
        const float4 state = tex2Dlod(LAYER_NAME(Layer_StateSampler_), float4(0.25, 0.5, 0.0, 0.0));
        pos = state.xy;
        angle = state.z;
        scale = exp2(state.w);
    }
#endif
}

float2 Layer_Size(float scale) { return float2(LAYER_SIZE_X, LAYER_SIZE_Y) * scale; }

// The layer as two triangles: its corners rotated around the pivot, placed so that Layer_Pos 0 / 1 puts it at the
// left / right (top / bottom) edge of the screen as in version 0.2.
void Layer_VS(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0)
{
    texcoord = float2(id == 1 || id == 4 || id == 5 ? 1.0 : 0.0, id == 2 || id == 3 || id == 5 ? 1.0 : 0.0);

    float2 pos;
    float angle, scale;
    Layer_Placement(pos, angle, scale);

    const float2 size = Layer_Size(scale);
    const float2 pivot = pos * (BUFFER_SCREEN_SIZE - size) + size * Layer_Pivot;
    const float2 local = (texcoord - Layer_Pivot) * size;
    float s, c;
    sincos(radians(angle), s, c);
    const float2 pixel = pivot + float2(local.x * c - local.y * s, local.x * s + local.y * c);

    position = float4(pixel * BUFFER_PIXEL_SIZE * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

#if LAYER_BLEND_STATE == 0
float4 Layer_PS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
    const float4 layer = tex2D(LAYER_NAME(Layer_Sampler_), texcoord);
    const float4 back  = tex2D(ReShade::BackBuffer, vpos.xy * BUFFER_PIXEL_SIZE);
    return float4(BlendModes::Blend(Layer_Mode, back.rgb, layer.rgb, layer.a * Layer_Blend), back.a);
}
#define LAYER_PASS_STATE
#else
// The source for the blend stage (the game image is not read).
float4 Layer_PS(float4 vpos : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
    const float4 layer = tex2D(LAYER_NAME(Layer_Sampler_), texcoord);
    const float k = layer.a * Layer_Blend;
#if LAYER_BLEND_STATE == 1
    return BlendModes::Source_Normal(layer.rgb, k);
#elif LAYER_BLEND_STATE == 2
    return BlendModes::Source_Multiply(layer.rgb, k);
#elif LAYER_BLEND_STATE == 3
    return BlendModes::Source_Screen(layer.rgb, k);
#elif LAYER_BLEND_STATE == 4
    return BlendModes::Source_Add(layer.rgb, k);
#elif LAYER_BLEND_STATE == 5
    return BlendModes::Source_Darken(k >= 0.5 ? layer.rgb : 1.0);
#elif LAYER_BLEND_STATE == 6
    return BlendModes::Source_Lighten(k >= 0.5 ? layer.rgb : 0.0);
#elif LAYER_BLEND_STATE == 7
    return BlendModes::Source_LinearBurn(layer.rgb, k);
#elif LAYER_BLEND_STATE == 8
    return BlendModes::Source_Exclusion(layer.rgb, k);
#else
    return BlendModes::Source_Subtract(layer.rgb, k);
#endif
}
#if LAYER_BLEND_STATE == 1
    #define LAYER_PASS_STATE BLENDMODES_STATE_NORMAL
#elif LAYER_BLEND_STATE == 2
    #define LAYER_PASS_STATE BLENDMODES_STATE_MULTIPLY
#elif LAYER_BLEND_STATE == 3
    #define LAYER_PASS_STATE BLENDMODES_STATE_SCREEN
#elif LAYER_BLEND_STATE == 4
    #define LAYER_PASS_STATE BLENDMODES_STATE_ADD
#elif LAYER_BLEND_STATE == 5
    #define LAYER_PASS_STATE BLENDMODES_STATE_DARKEN
#elif LAYER_BLEND_STATE == 6
    #define LAYER_PASS_STATE BLENDMODES_STATE_LIGHTEN
#elif LAYER_BLEND_STATE == 7
    #define LAYER_PASS_STATE BLENDMODES_STATE_LINEARBURN
#elif LAYER_BLEND_STATE == 8
    #define LAYER_PASS_STATE BLENDMODES_STATE_EXCLUSION
#else
    #define LAYER_PASS_STATE BLENDMODES_STATE_SUBTRACT
#endif
#endif

#if LAYER_MOUSE
float4 Layer_PS_KeepState(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    return tex2Dfetch(LAYER_NAME(Layer_StateSampler_), int2(vpos.xy));
}

float4 Layer_PS_State(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float4 sliders = float4(Layer_Pos, Layer_Angle, Layer_Scale);
    if (!Layer_Mouse)
        return float4(-1e30, -1e30, -1e30, -1e30); // not in use: the next start takes the slider values

    float4 state = tex2Dfetch(LAYER_NAME(Layer_StatePrevSampler_), int2(0, 0));
    const float4 taken = tex2Dfetch(LAYER_NAME(Layer_StatePrevSampler_), int2(1, 0));

    if (any(taken != sliders) || Layer_MiddleClick)
        state = float4(Layer_Pos, Layer_Angle, log2(Layer_Scale));
    else if (Layer_OverlayOpen && !Layer_OverlayHovered)
    {
        if (Layer_LeftButton)
        {
            // Layer_Pos moves the layer over BUFFER_SCREEN_SIZE - size pixels.
            float2 travel = BUFFER_SCREEN_SIZE - Layer_Size(exp2(state.w));
            travel = abs(travel) < 1.0 ? 1.0 : travel;
            state.xy += Layer_MouseDelta / travel;
        }
        if (Layer_RightButton)
        {
            state.z += Layer_MouseDelta.x * 0.25;
            state.z -= 360.0 * floor((state.z + 180.0) / 360.0); // back into [-180, 180)
        }
        state.w = clamp(state.w + Layer_MouseWheel.y * 0.05, -8.0, 4.0);
    }
    return vpos.x < 1.0 ? state : sliders;
}

// The values next to the mouse: "X 0.500 Y 0.500 A 0.0 S 1.000" in a 3 x 5 pixel font, each pixel 2 x 2.
static const uint kLayerFont[16] = { 31599u, 11415u, 29671u, 29647u, 23497u, 31183u, 31215u, 29257u, 31727u, 31695u,
                                     2u, 448u, 23213u, 23186u, 11245u, 14478u };   // 0-9 . - X Y A S
static const uint kLayerPow10[6] = { 1u, 10u, 100u, 1000u, 10000u, 100000u };
#define LAYER_TEXT_CHARS 30
#define LAYER_TEXT_SCALE 2

// Character j of a field: [sign] digits . decimals; -1 = blank.
int Layer_FieldChar(int j, float value, int digits, int decimals, bool isSigned)
{
    if (isSigned)
    {
        if (j == 0)
            return value < 0.0 ? 11 : -1;
        j -= 1;
    }
    const uint n = uint(round(abs(value) * kLayerPow10[decimals]));
    if (j < digits)
        return int((n / kLayerPow10[decimals + digits - 1 - j]) % 10u);
    if (j == digits)
        return 10;
    return int((n / kLayerPow10[decimals - 1 - (j - digits - 1)]) % 10u);
}

int Layer_TextChar(int i, float4 state)
{
    // X s d . d d d _ Y s d . d d d _ A s d d d . d _ S d . d d d
    if (i == 0) return 12;
    if (i < 7) return Layer_FieldChar(i - 1, state.x, 1, 3, true);
    if (i == 8) return 13;
    if (i > 8 && i < 15) return Layer_FieldChar(i - 9, state.y, 1, 3, true);
    if (i == 16) return 14;
    if (i > 16 && i < 23) return Layer_FieldChar(i - 17, state.z, 3, 1, true);
    if (i == 24) return 15;
    if (i > 24) return Layer_FieldChar(i - 25, exp2(state.w), 1, 3, false);
    return -1;
}

void Layer_VS_Text(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0)
{
    texcoord = float2(id == 1 || id == 4 || id == 5 ? 1.0 : 0.0, id == 2 || id == 3 || id == 5 ? 1.0 : 0.0);
    const float2 size = float2(LAYER_TEXT_CHARS * 4 + 2, 8) * LAYER_TEXT_SCALE;
    const float2 pixel = Layer_MousePoint + 16.0 + texcoord * size;
    position = float4(pixel * BUFFER_PIXEL_SIZE * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    if (!Layer_Mouse || !Layer_OverlayOpen)
        position = 0.0; // nothing to draw
}

float4 Layer_PS_Text(float4 vpos : SV_Position, float2 texcoord : TEXCOORD0) : SV_Target
{
    const int2 p = int2(floor((vpos.xy - (Layer_MousePoint + 16.0)) / LAYER_TEXT_SCALE)) - 1;  // 1 pixel margin
    const int i = p.x / 4, x = p.x % 4, y = p.y;
    bool on = false;
    if (p.x >= 0 && p.y >= 0 && x < 3 && y < 5 && i < LAYER_TEXT_CHARS)
    {
        const int g = Layer_TextChar(i, tex2Dfetch(LAYER_NAME(Layer_StateSampler_), int2(0, 0)));
        on = g >= 0 && ((kLayerFont[g] >> uint((4 - y) * 3 + (2 - x))) & 1u) != 0u;
    }
    return on ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
}
#endif

technique Layer < ui_label = __FILE_STEM__; ui_tooltip = "Blends an image with the game (" LAYER_SOURCE ")."; >
{
#if LAYER_MOUSE
    pass LayerKeepState
    {
        VertexShader = PostProcessVS;
        PixelShader  = Layer_PS_KeepState;
        RenderTarget = LAYER_NAME(Layer_StatePrev_);
    }
    pass LayerState
    {
        VertexShader = PostProcessVS;
        PixelShader  = Layer_PS_State;
        RenderTarget = LAYER_NAME(Layer_State_);
    }
#endif
    pass LayerDraw
    {
        VertexShader = Layer_VS;
        PixelShader  = Layer_PS;
        VertexCount  = 6;
        LAYER_PASS_STATE
    }
#if LAYER_MOUSE
    pass LayerText
    {
        VertexShader = Layer_VS_Text;
        PixelShader  = Layer_PS_Text;
        VertexCount  = 6;
    }
#endif
}
