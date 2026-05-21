/* Register-pressure stress fixture (value spilling, straight-line).
 *
 * 300 values v0..v299 are each loaded from a zero-initialised global array
 * `g` (so clang cannot constant-fold or dead-code them across the opaque
 * global) and offset by n*(k+1). The return value is the palindrome sum
 *     sum_{k=0}^{299} v_k * v_{299-k}
 * Pairing index k with 299-k means no prefix of the sum can be reduced
 * before all values exist, so all 300 values are simultaneously live ->
 * the linear allocator must spill the excess (ssa_high pins at R230, ~81
 * values spilled). g[] stays zero at runtime, so v_k = n*(k+1) and the
 * result is exactly  n*n * sum_k (k+1)*(300-k)  in i32 wraparound.
 *
 * This file is generated; see the build comment in CMakeLists.txt. Expected
 * values (i32 two's-complement) are checked in the e2e tests.
 */

int g[300];

int vm_main(int n) {
    int v0 = g[0] + n * 1;
    int v1 = g[1] + n * 2;
    int v2 = g[2] + n * 3;
    int v3 = g[3] + n * 4;
    int v4 = g[4] + n * 5;
    int v5 = g[5] + n * 6;
    int v6 = g[6] + n * 7;
    int v7 = g[7] + n * 8;
    int v8 = g[8] + n * 9;
    int v9 = g[9] + n * 10;
    int v10 = g[10] + n * 11;
    int v11 = g[11] + n * 12;
    int v12 = g[12] + n * 13;
    int v13 = g[13] + n * 14;
    int v14 = g[14] + n * 15;
    int v15 = g[15] + n * 16;
    int v16 = g[16] + n * 17;
    int v17 = g[17] + n * 18;
    int v18 = g[18] + n * 19;
    int v19 = g[19] + n * 20;
    int v20 = g[20] + n * 21;
    int v21 = g[21] + n * 22;
    int v22 = g[22] + n * 23;
    int v23 = g[23] + n * 24;
    int v24 = g[24] + n * 25;
    int v25 = g[25] + n * 26;
    int v26 = g[26] + n * 27;
    int v27 = g[27] + n * 28;
    int v28 = g[28] + n * 29;
    int v29 = g[29] + n * 30;
    int v30 = g[30] + n * 31;
    int v31 = g[31] + n * 32;
    int v32 = g[32] + n * 33;
    int v33 = g[33] + n * 34;
    int v34 = g[34] + n * 35;
    int v35 = g[35] + n * 36;
    int v36 = g[36] + n * 37;
    int v37 = g[37] + n * 38;
    int v38 = g[38] + n * 39;
    int v39 = g[39] + n * 40;
    int v40 = g[40] + n * 41;
    int v41 = g[41] + n * 42;
    int v42 = g[42] + n * 43;
    int v43 = g[43] + n * 44;
    int v44 = g[44] + n * 45;
    int v45 = g[45] + n * 46;
    int v46 = g[46] + n * 47;
    int v47 = g[47] + n * 48;
    int v48 = g[48] + n * 49;
    int v49 = g[49] + n * 50;
    int v50 = g[50] + n * 51;
    int v51 = g[51] + n * 52;
    int v52 = g[52] + n * 53;
    int v53 = g[53] + n * 54;
    int v54 = g[54] + n * 55;
    int v55 = g[55] + n * 56;
    int v56 = g[56] + n * 57;
    int v57 = g[57] + n * 58;
    int v58 = g[58] + n * 59;
    int v59 = g[59] + n * 60;
    int v60 = g[60] + n * 61;
    int v61 = g[61] + n * 62;
    int v62 = g[62] + n * 63;
    int v63 = g[63] + n * 64;
    int v64 = g[64] + n * 65;
    int v65 = g[65] + n * 66;
    int v66 = g[66] + n * 67;
    int v67 = g[67] + n * 68;
    int v68 = g[68] + n * 69;
    int v69 = g[69] + n * 70;
    int v70 = g[70] + n * 71;
    int v71 = g[71] + n * 72;
    int v72 = g[72] + n * 73;
    int v73 = g[73] + n * 74;
    int v74 = g[74] + n * 75;
    int v75 = g[75] + n * 76;
    int v76 = g[76] + n * 77;
    int v77 = g[77] + n * 78;
    int v78 = g[78] + n * 79;
    int v79 = g[79] + n * 80;
    int v80 = g[80] + n * 81;
    int v81 = g[81] + n * 82;
    int v82 = g[82] + n * 83;
    int v83 = g[83] + n * 84;
    int v84 = g[84] + n * 85;
    int v85 = g[85] + n * 86;
    int v86 = g[86] + n * 87;
    int v87 = g[87] + n * 88;
    int v88 = g[88] + n * 89;
    int v89 = g[89] + n * 90;
    int v90 = g[90] + n * 91;
    int v91 = g[91] + n * 92;
    int v92 = g[92] + n * 93;
    int v93 = g[93] + n * 94;
    int v94 = g[94] + n * 95;
    int v95 = g[95] + n * 96;
    int v96 = g[96] + n * 97;
    int v97 = g[97] + n * 98;
    int v98 = g[98] + n * 99;
    int v99 = g[99] + n * 100;
    int v100 = g[100] + n * 101;
    int v101 = g[101] + n * 102;
    int v102 = g[102] + n * 103;
    int v103 = g[103] + n * 104;
    int v104 = g[104] + n * 105;
    int v105 = g[105] + n * 106;
    int v106 = g[106] + n * 107;
    int v107 = g[107] + n * 108;
    int v108 = g[108] + n * 109;
    int v109 = g[109] + n * 110;
    int v110 = g[110] + n * 111;
    int v111 = g[111] + n * 112;
    int v112 = g[112] + n * 113;
    int v113 = g[113] + n * 114;
    int v114 = g[114] + n * 115;
    int v115 = g[115] + n * 116;
    int v116 = g[116] + n * 117;
    int v117 = g[117] + n * 118;
    int v118 = g[118] + n * 119;
    int v119 = g[119] + n * 120;
    int v120 = g[120] + n * 121;
    int v121 = g[121] + n * 122;
    int v122 = g[122] + n * 123;
    int v123 = g[123] + n * 124;
    int v124 = g[124] + n * 125;
    int v125 = g[125] + n * 126;
    int v126 = g[126] + n * 127;
    int v127 = g[127] + n * 128;
    int v128 = g[128] + n * 129;
    int v129 = g[129] + n * 130;
    int v130 = g[130] + n * 131;
    int v131 = g[131] + n * 132;
    int v132 = g[132] + n * 133;
    int v133 = g[133] + n * 134;
    int v134 = g[134] + n * 135;
    int v135 = g[135] + n * 136;
    int v136 = g[136] + n * 137;
    int v137 = g[137] + n * 138;
    int v138 = g[138] + n * 139;
    int v139 = g[139] + n * 140;
    int v140 = g[140] + n * 141;
    int v141 = g[141] + n * 142;
    int v142 = g[142] + n * 143;
    int v143 = g[143] + n * 144;
    int v144 = g[144] + n * 145;
    int v145 = g[145] + n * 146;
    int v146 = g[146] + n * 147;
    int v147 = g[147] + n * 148;
    int v148 = g[148] + n * 149;
    int v149 = g[149] + n * 150;
    int v150 = g[150] + n * 151;
    int v151 = g[151] + n * 152;
    int v152 = g[152] + n * 153;
    int v153 = g[153] + n * 154;
    int v154 = g[154] + n * 155;
    int v155 = g[155] + n * 156;
    int v156 = g[156] + n * 157;
    int v157 = g[157] + n * 158;
    int v158 = g[158] + n * 159;
    int v159 = g[159] + n * 160;
    int v160 = g[160] + n * 161;
    int v161 = g[161] + n * 162;
    int v162 = g[162] + n * 163;
    int v163 = g[163] + n * 164;
    int v164 = g[164] + n * 165;
    int v165 = g[165] + n * 166;
    int v166 = g[166] + n * 167;
    int v167 = g[167] + n * 168;
    int v168 = g[168] + n * 169;
    int v169 = g[169] + n * 170;
    int v170 = g[170] + n * 171;
    int v171 = g[171] + n * 172;
    int v172 = g[172] + n * 173;
    int v173 = g[173] + n * 174;
    int v174 = g[174] + n * 175;
    int v175 = g[175] + n * 176;
    int v176 = g[176] + n * 177;
    int v177 = g[177] + n * 178;
    int v178 = g[178] + n * 179;
    int v179 = g[179] + n * 180;
    int v180 = g[180] + n * 181;
    int v181 = g[181] + n * 182;
    int v182 = g[182] + n * 183;
    int v183 = g[183] + n * 184;
    int v184 = g[184] + n * 185;
    int v185 = g[185] + n * 186;
    int v186 = g[186] + n * 187;
    int v187 = g[187] + n * 188;
    int v188 = g[188] + n * 189;
    int v189 = g[189] + n * 190;
    int v190 = g[190] + n * 191;
    int v191 = g[191] + n * 192;
    int v192 = g[192] + n * 193;
    int v193 = g[193] + n * 194;
    int v194 = g[194] + n * 195;
    int v195 = g[195] + n * 196;
    int v196 = g[196] + n * 197;
    int v197 = g[197] + n * 198;
    int v198 = g[198] + n * 199;
    int v199 = g[199] + n * 200;
    int v200 = g[200] + n * 201;
    int v201 = g[201] + n * 202;
    int v202 = g[202] + n * 203;
    int v203 = g[203] + n * 204;
    int v204 = g[204] + n * 205;
    int v205 = g[205] + n * 206;
    int v206 = g[206] + n * 207;
    int v207 = g[207] + n * 208;
    int v208 = g[208] + n * 209;
    int v209 = g[209] + n * 210;
    int v210 = g[210] + n * 211;
    int v211 = g[211] + n * 212;
    int v212 = g[212] + n * 213;
    int v213 = g[213] + n * 214;
    int v214 = g[214] + n * 215;
    int v215 = g[215] + n * 216;
    int v216 = g[216] + n * 217;
    int v217 = g[217] + n * 218;
    int v218 = g[218] + n * 219;
    int v219 = g[219] + n * 220;
    int v220 = g[220] + n * 221;
    int v221 = g[221] + n * 222;
    int v222 = g[222] + n * 223;
    int v223 = g[223] + n * 224;
    int v224 = g[224] + n * 225;
    int v225 = g[225] + n * 226;
    int v226 = g[226] + n * 227;
    int v227 = g[227] + n * 228;
    int v228 = g[228] + n * 229;
    int v229 = g[229] + n * 230;
    int v230 = g[230] + n * 231;
    int v231 = g[231] + n * 232;
    int v232 = g[232] + n * 233;
    int v233 = g[233] + n * 234;
    int v234 = g[234] + n * 235;
    int v235 = g[235] + n * 236;
    int v236 = g[236] + n * 237;
    int v237 = g[237] + n * 238;
    int v238 = g[238] + n * 239;
    int v239 = g[239] + n * 240;
    int v240 = g[240] + n * 241;
    int v241 = g[241] + n * 242;
    int v242 = g[242] + n * 243;
    int v243 = g[243] + n * 244;
    int v244 = g[244] + n * 245;
    int v245 = g[245] + n * 246;
    int v246 = g[246] + n * 247;
    int v247 = g[247] + n * 248;
    int v248 = g[248] + n * 249;
    int v249 = g[249] + n * 250;
    int v250 = g[250] + n * 251;
    int v251 = g[251] + n * 252;
    int v252 = g[252] + n * 253;
    int v253 = g[253] + n * 254;
    int v254 = g[254] + n * 255;
    int v255 = g[255] + n * 256;
    int v256 = g[256] + n * 257;
    int v257 = g[257] + n * 258;
    int v258 = g[258] + n * 259;
    int v259 = g[259] + n * 260;
    int v260 = g[260] + n * 261;
    int v261 = g[261] + n * 262;
    int v262 = g[262] + n * 263;
    int v263 = g[263] + n * 264;
    int v264 = g[264] + n * 265;
    int v265 = g[265] + n * 266;
    int v266 = g[266] + n * 267;
    int v267 = g[267] + n * 268;
    int v268 = g[268] + n * 269;
    int v269 = g[269] + n * 270;
    int v270 = g[270] + n * 271;
    int v271 = g[271] + n * 272;
    int v272 = g[272] + n * 273;
    int v273 = g[273] + n * 274;
    int v274 = g[274] + n * 275;
    int v275 = g[275] + n * 276;
    int v276 = g[276] + n * 277;
    int v277 = g[277] + n * 278;
    int v278 = g[278] + n * 279;
    int v279 = g[279] + n * 280;
    int v280 = g[280] + n * 281;
    int v281 = g[281] + n * 282;
    int v282 = g[282] + n * 283;
    int v283 = g[283] + n * 284;
    int v284 = g[284] + n * 285;
    int v285 = g[285] + n * 286;
    int v286 = g[286] + n * 287;
    int v287 = g[287] + n * 288;
    int v288 = g[288] + n * 289;
    int v289 = g[289] + n * 290;
    int v290 = g[290] + n * 291;
    int v291 = g[291] + n * 292;
    int v292 = g[292] + n * 293;
    int v293 = g[293] + n * 294;
    int v294 = g[294] + n * 295;
    int v295 = g[295] + n * 296;
    int v296 = g[296] + n * 297;
    int v297 = g[297] + n * 298;
    int v298 = g[298] + n * 299;
    int v299 = g[299] + n * 300;
    return
          v0 * v299
        + v1 * v298
        + v2 * v297
        + v3 * v296
        + v4 * v295
        + v5 * v294
        + v6 * v293
        + v7 * v292
        + v8 * v291
        + v9 * v290
        + v10 * v289
        + v11 * v288
        + v12 * v287
        + v13 * v286
        + v14 * v285
        + v15 * v284
        + v16 * v283
        + v17 * v282
        + v18 * v281
        + v19 * v280
        + v20 * v279
        + v21 * v278
        + v22 * v277
        + v23 * v276
        + v24 * v275
        + v25 * v274
        + v26 * v273
        + v27 * v272
        + v28 * v271
        + v29 * v270
        + v30 * v269
        + v31 * v268
        + v32 * v267
        + v33 * v266
        + v34 * v265
        + v35 * v264
        + v36 * v263
        + v37 * v262
        + v38 * v261
        + v39 * v260
        + v40 * v259
        + v41 * v258
        + v42 * v257
        + v43 * v256
        + v44 * v255
        + v45 * v254
        + v46 * v253
        + v47 * v252
        + v48 * v251
        + v49 * v250
        + v50 * v249
        + v51 * v248
        + v52 * v247
        + v53 * v246
        + v54 * v245
        + v55 * v244
        + v56 * v243
        + v57 * v242
        + v58 * v241
        + v59 * v240
        + v60 * v239
        + v61 * v238
        + v62 * v237
        + v63 * v236
        + v64 * v235
        + v65 * v234
        + v66 * v233
        + v67 * v232
        + v68 * v231
        + v69 * v230
        + v70 * v229
        + v71 * v228
        + v72 * v227
        + v73 * v226
        + v74 * v225
        + v75 * v224
        + v76 * v223
        + v77 * v222
        + v78 * v221
        + v79 * v220
        + v80 * v219
        + v81 * v218
        + v82 * v217
        + v83 * v216
        + v84 * v215
        + v85 * v214
        + v86 * v213
        + v87 * v212
        + v88 * v211
        + v89 * v210
        + v90 * v209
        + v91 * v208
        + v92 * v207
        + v93 * v206
        + v94 * v205
        + v95 * v204
        + v96 * v203
        + v97 * v202
        + v98 * v201
        + v99 * v200
        + v100 * v199
        + v101 * v198
        + v102 * v197
        + v103 * v196
        + v104 * v195
        + v105 * v194
        + v106 * v193
        + v107 * v192
        + v108 * v191
        + v109 * v190
        + v110 * v189
        + v111 * v188
        + v112 * v187
        + v113 * v186
        + v114 * v185
        + v115 * v184
        + v116 * v183
        + v117 * v182
        + v118 * v181
        + v119 * v180
        + v120 * v179
        + v121 * v178
        + v122 * v177
        + v123 * v176
        + v124 * v175
        + v125 * v174
        + v126 * v173
        + v127 * v172
        + v128 * v171
        + v129 * v170
        + v130 * v169
        + v131 * v168
        + v132 * v167
        + v133 * v166
        + v134 * v165
        + v135 * v164
        + v136 * v163
        + v137 * v162
        + v138 * v161
        + v139 * v160
        + v140 * v159
        + v141 * v158
        + v142 * v157
        + v143 * v156
        + v144 * v155
        + v145 * v154
        + v146 * v153
        + v147 * v152
        + v148 * v151
        + v149 * v150
        + v150 * v149
        + v151 * v148
        + v152 * v147
        + v153 * v146
        + v154 * v145
        + v155 * v144
        + v156 * v143
        + v157 * v142
        + v158 * v141
        + v159 * v140
        + v160 * v139
        + v161 * v138
        + v162 * v137
        + v163 * v136
        + v164 * v135
        + v165 * v134
        + v166 * v133
        + v167 * v132
        + v168 * v131
        + v169 * v130
        + v170 * v129
        + v171 * v128
        + v172 * v127
        + v173 * v126
        + v174 * v125
        + v175 * v124
        + v176 * v123
        + v177 * v122
        + v178 * v121
        + v179 * v120
        + v180 * v119
        + v181 * v118
        + v182 * v117
        + v183 * v116
        + v184 * v115
        + v185 * v114
        + v186 * v113
        + v187 * v112
        + v188 * v111
        + v189 * v110
        + v190 * v109
        + v191 * v108
        + v192 * v107
        + v193 * v106
        + v194 * v105
        + v195 * v104
        + v196 * v103
        + v197 * v102
        + v198 * v101
        + v199 * v100
        + v200 * v99
        + v201 * v98
        + v202 * v97
        + v203 * v96
        + v204 * v95
        + v205 * v94
        + v206 * v93
        + v207 * v92
        + v208 * v91
        + v209 * v90
        + v210 * v89
        + v211 * v88
        + v212 * v87
        + v213 * v86
        + v214 * v85
        + v215 * v84
        + v216 * v83
        + v217 * v82
        + v218 * v81
        + v219 * v80
        + v220 * v79
        + v221 * v78
        + v222 * v77
        + v223 * v76
        + v224 * v75
        + v225 * v74
        + v226 * v73
        + v227 * v72
        + v228 * v71
        + v229 * v70
        + v230 * v69
        + v231 * v68
        + v232 * v67
        + v233 * v66
        + v234 * v65
        + v235 * v64
        + v236 * v63
        + v237 * v62
        + v238 * v61
        + v239 * v60
        + v240 * v59
        + v241 * v58
        + v242 * v57
        + v243 * v56
        + v244 * v55
        + v245 * v54
        + v246 * v53
        + v247 * v52
        + v248 * v51
        + v249 * v50
        + v250 * v49
        + v251 * v48
        + v252 * v47
        + v253 * v46
        + v254 * v45
        + v255 * v44
        + v256 * v43
        + v257 * v42
        + v258 * v41
        + v259 * v40
        + v260 * v39
        + v261 * v38
        + v262 * v37
        + v263 * v36
        + v264 * v35
        + v265 * v34
        + v266 * v33
        + v267 * v32
        + v268 * v31
        + v269 * v30
        + v270 * v29
        + v271 * v28
        + v272 * v27
        + v273 * v26
        + v274 * v25
        + v275 * v24
        + v276 * v23
        + v277 * v22
        + v278 * v21
        + v279 * v20
        + v280 * v19
        + v281 * v18
        + v282 * v17
        + v283 * v16
        + v284 * v15
        + v285 * v14
        + v286 * v13
        + v287 * v12
        + v288 * v11
        + v289 * v10
        + v290 * v9
        + v291 * v8
        + v292 * v7
        + v293 * v6
        + v294 * v5
        + v295 * v4
        + v296 * v3
        + v297 * v2
        + v298 * v1
        + v299 * v0;
}
