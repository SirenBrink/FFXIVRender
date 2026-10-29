#pragma once
// Shared by real-frame HDR output and the FG HUDless image. Apply effects
// after compressing scene highlights; changing this order pumps exposure
// between generated and real frames. Inputs are linear RGB, output is nits.
#define XIVRENDER_HDR_COMPOSITION_HLSL                                                                       \
    "float3 hdr_compose(float3 displayed,float3 scene,float3 reference,float trans,float3 before,uint "      \
    "effects,float paper,float peak){"                                                                       \
    "scene*=paper;float p=max(scene.r,max(scene.g,scene.b));if(p>paper){float "                              \
    "e=p-paper,span=max(peak-paper,.001);scene*=(paper+span*e/(span+e))/p;}"                                 \
    "float3 gain=1;if(effects)gain=before>1e-5?clamp(displayed/max(before,1e-5),0,8):0;"                     \
    "return displayed*paper+trans*(scene-reference*paper)*gain;}"
