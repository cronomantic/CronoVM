/* Register-pressure stress fixture (value spilling ACROSS blocks / phis).
 *
 * 260 carriers c0..cN are initialised from a zero-init global array `g`
 * (opaque, defeats folding) and then updated inside a counted loop. Each
 * carrier is live across the loop back-edge, so all N are simultaneously
 * live throughout the loop body -> they exceed the register file and many
 * are spilled. Because each carrier is a loop-carried value, clang emits a
 * PHI for it at the loop header; spilling those carriers exercises the
 * spilled-PHI store/reload path (a classic spill-bug source).
 *
 * Update rule per iteration:  c_k = c_k + (k + 1)
 * After `n` iterations:  c_k = g[k] + n*(k+1)  (g[k]==0 at runtime)
 * Return palindrome sum  sum_k c_k * c_{N-1-k}  =  same closed form as
 * spill_many but reached through a loop, so the cross-block liveness is
 * real. Exact i32 wraparound; expected values checked in the e2e tests.
 *
 * Generated file; see CMakeLists.txt.
 */

int g[260];

int vm_main(int n) {
    int c0 = g[0];
    int c1 = g[1];
    int c2 = g[2];
    int c3 = g[3];
    int c4 = g[4];
    int c5 = g[5];
    int c6 = g[6];
    int c7 = g[7];
    int c8 = g[8];
    int c9 = g[9];
    int c10 = g[10];
    int c11 = g[11];
    int c12 = g[12];
    int c13 = g[13];
    int c14 = g[14];
    int c15 = g[15];
    int c16 = g[16];
    int c17 = g[17];
    int c18 = g[18];
    int c19 = g[19];
    int c20 = g[20];
    int c21 = g[21];
    int c22 = g[22];
    int c23 = g[23];
    int c24 = g[24];
    int c25 = g[25];
    int c26 = g[26];
    int c27 = g[27];
    int c28 = g[28];
    int c29 = g[29];
    int c30 = g[30];
    int c31 = g[31];
    int c32 = g[32];
    int c33 = g[33];
    int c34 = g[34];
    int c35 = g[35];
    int c36 = g[36];
    int c37 = g[37];
    int c38 = g[38];
    int c39 = g[39];
    int c40 = g[40];
    int c41 = g[41];
    int c42 = g[42];
    int c43 = g[43];
    int c44 = g[44];
    int c45 = g[45];
    int c46 = g[46];
    int c47 = g[47];
    int c48 = g[48];
    int c49 = g[49];
    int c50 = g[50];
    int c51 = g[51];
    int c52 = g[52];
    int c53 = g[53];
    int c54 = g[54];
    int c55 = g[55];
    int c56 = g[56];
    int c57 = g[57];
    int c58 = g[58];
    int c59 = g[59];
    int c60 = g[60];
    int c61 = g[61];
    int c62 = g[62];
    int c63 = g[63];
    int c64 = g[64];
    int c65 = g[65];
    int c66 = g[66];
    int c67 = g[67];
    int c68 = g[68];
    int c69 = g[69];
    int c70 = g[70];
    int c71 = g[71];
    int c72 = g[72];
    int c73 = g[73];
    int c74 = g[74];
    int c75 = g[75];
    int c76 = g[76];
    int c77 = g[77];
    int c78 = g[78];
    int c79 = g[79];
    int c80 = g[80];
    int c81 = g[81];
    int c82 = g[82];
    int c83 = g[83];
    int c84 = g[84];
    int c85 = g[85];
    int c86 = g[86];
    int c87 = g[87];
    int c88 = g[88];
    int c89 = g[89];
    int c90 = g[90];
    int c91 = g[91];
    int c92 = g[92];
    int c93 = g[93];
    int c94 = g[94];
    int c95 = g[95];
    int c96 = g[96];
    int c97 = g[97];
    int c98 = g[98];
    int c99 = g[99];
    int c100 = g[100];
    int c101 = g[101];
    int c102 = g[102];
    int c103 = g[103];
    int c104 = g[104];
    int c105 = g[105];
    int c106 = g[106];
    int c107 = g[107];
    int c108 = g[108];
    int c109 = g[109];
    int c110 = g[110];
    int c111 = g[111];
    int c112 = g[112];
    int c113 = g[113];
    int c114 = g[114];
    int c115 = g[115];
    int c116 = g[116];
    int c117 = g[117];
    int c118 = g[118];
    int c119 = g[119];
    int c120 = g[120];
    int c121 = g[121];
    int c122 = g[122];
    int c123 = g[123];
    int c124 = g[124];
    int c125 = g[125];
    int c126 = g[126];
    int c127 = g[127];
    int c128 = g[128];
    int c129 = g[129];
    int c130 = g[130];
    int c131 = g[131];
    int c132 = g[132];
    int c133 = g[133];
    int c134 = g[134];
    int c135 = g[135];
    int c136 = g[136];
    int c137 = g[137];
    int c138 = g[138];
    int c139 = g[139];
    int c140 = g[140];
    int c141 = g[141];
    int c142 = g[142];
    int c143 = g[143];
    int c144 = g[144];
    int c145 = g[145];
    int c146 = g[146];
    int c147 = g[147];
    int c148 = g[148];
    int c149 = g[149];
    int c150 = g[150];
    int c151 = g[151];
    int c152 = g[152];
    int c153 = g[153];
    int c154 = g[154];
    int c155 = g[155];
    int c156 = g[156];
    int c157 = g[157];
    int c158 = g[158];
    int c159 = g[159];
    int c160 = g[160];
    int c161 = g[161];
    int c162 = g[162];
    int c163 = g[163];
    int c164 = g[164];
    int c165 = g[165];
    int c166 = g[166];
    int c167 = g[167];
    int c168 = g[168];
    int c169 = g[169];
    int c170 = g[170];
    int c171 = g[171];
    int c172 = g[172];
    int c173 = g[173];
    int c174 = g[174];
    int c175 = g[175];
    int c176 = g[176];
    int c177 = g[177];
    int c178 = g[178];
    int c179 = g[179];
    int c180 = g[180];
    int c181 = g[181];
    int c182 = g[182];
    int c183 = g[183];
    int c184 = g[184];
    int c185 = g[185];
    int c186 = g[186];
    int c187 = g[187];
    int c188 = g[188];
    int c189 = g[189];
    int c190 = g[190];
    int c191 = g[191];
    int c192 = g[192];
    int c193 = g[193];
    int c194 = g[194];
    int c195 = g[195];
    int c196 = g[196];
    int c197 = g[197];
    int c198 = g[198];
    int c199 = g[199];
    int c200 = g[200];
    int c201 = g[201];
    int c202 = g[202];
    int c203 = g[203];
    int c204 = g[204];
    int c205 = g[205];
    int c206 = g[206];
    int c207 = g[207];
    int c208 = g[208];
    int c209 = g[209];
    int c210 = g[210];
    int c211 = g[211];
    int c212 = g[212];
    int c213 = g[213];
    int c214 = g[214];
    int c215 = g[215];
    int c216 = g[216];
    int c217 = g[217];
    int c218 = g[218];
    int c219 = g[219];
    int c220 = g[220];
    int c221 = g[221];
    int c222 = g[222];
    int c223 = g[223];
    int c224 = g[224];
    int c225 = g[225];
    int c226 = g[226];
    int c227 = g[227];
    int c228 = g[228];
    int c229 = g[229];
    int c230 = g[230];
    int c231 = g[231];
    int c232 = g[232];
    int c233 = g[233];
    int c234 = g[234];
    int c235 = g[235];
    int c236 = g[236];
    int c237 = g[237];
    int c238 = g[238];
    int c239 = g[239];
    int c240 = g[240];
    int c241 = g[241];
    int c242 = g[242];
    int c243 = g[243];
    int c244 = g[244];
    int c245 = g[245];
    int c246 = g[246];
    int c247 = g[247];
    int c248 = g[248];
    int c249 = g[249];
    int c250 = g[250];
    int c251 = g[251];
    int c252 = g[252];
    int c253 = g[253];
    int c254 = g[254];
    int c255 = g[255];
    int c256 = g[256];
    int c257 = g[257];
    int c258 = g[258];
    int c259 = g[259];
    for (int i = 0; i < n; i++) {
        c0 += 1;
        c1 += 2;
        c2 += 3;
        c3 += 4;
        c4 += 5;
        c5 += 6;
        c6 += 7;
        c7 += 8;
        c8 += 9;
        c9 += 10;
        c10 += 11;
        c11 += 12;
        c12 += 13;
        c13 += 14;
        c14 += 15;
        c15 += 16;
        c16 += 17;
        c17 += 18;
        c18 += 19;
        c19 += 20;
        c20 += 21;
        c21 += 22;
        c22 += 23;
        c23 += 24;
        c24 += 25;
        c25 += 26;
        c26 += 27;
        c27 += 28;
        c28 += 29;
        c29 += 30;
        c30 += 31;
        c31 += 32;
        c32 += 33;
        c33 += 34;
        c34 += 35;
        c35 += 36;
        c36 += 37;
        c37 += 38;
        c38 += 39;
        c39 += 40;
        c40 += 41;
        c41 += 42;
        c42 += 43;
        c43 += 44;
        c44 += 45;
        c45 += 46;
        c46 += 47;
        c47 += 48;
        c48 += 49;
        c49 += 50;
        c50 += 51;
        c51 += 52;
        c52 += 53;
        c53 += 54;
        c54 += 55;
        c55 += 56;
        c56 += 57;
        c57 += 58;
        c58 += 59;
        c59 += 60;
        c60 += 61;
        c61 += 62;
        c62 += 63;
        c63 += 64;
        c64 += 65;
        c65 += 66;
        c66 += 67;
        c67 += 68;
        c68 += 69;
        c69 += 70;
        c70 += 71;
        c71 += 72;
        c72 += 73;
        c73 += 74;
        c74 += 75;
        c75 += 76;
        c76 += 77;
        c77 += 78;
        c78 += 79;
        c79 += 80;
        c80 += 81;
        c81 += 82;
        c82 += 83;
        c83 += 84;
        c84 += 85;
        c85 += 86;
        c86 += 87;
        c87 += 88;
        c88 += 89;
        c89 += 90;
        c90 += 91;
        c91 += 92;
        c92 += 93;
        c93 += 94;
        c94 += 95;
        c95 += 96;
        c96 += 97;
        c97 += 98;
        c98 += 99;
        c99 += 100;
        c100 += 101;
        c101 += 102;
        c102 += 103;
        c103 += 104;
        c104 += 105;
        c105 += 106;
        c106 += 107;
        c107 += 108;
        c108 += 109;
        c109 += 110;
        c110 += 111;
        c111 += 112;
        c112 += 113;
        c113 += 114;
        c114 += 115;
        c115 += 116;
        c116 += 117;
        c117 += 118;
        c118 += 119;
        c119 += 120;
        c120 += 121;
        c121 += 122;
        c122 += 123;
        c123 += 124;
        c124 += 125;
        c125 += 126;
        c126 += 127;
        c127 += 128;
        c128 += 129;
        c129 += 130;
        c130 += 131;
        c131 += 132;
        c132 += 133;
        c133 += 134;
        c134 += 135;
        c135 += 136;
        c136 += 137;
        c137 += 138;
        c138 += 139;
        c139 += 140;
        c140 += 141;
        c141 += 142;
        c142 += 143;
        c143 += 144;
        c144 += 145;
        c145 += 146;
        c146 += 147;
        c147 += 148;
        c148 += 149;
        c149 += 150;
        c150 += 151;
        c151 += 152;
        c152 += 153;
        c153 += 154;
        c154 += 155;
        c155 += 156;
        c156 += 157;
        c157 += 158;
        c158 += 159;
        c159 += 160;
        c160 += 161;
        c161 += 162;
        c162 += 163;
        c163 += 164;
        c164 += 165;
        c165 += 166;
        c166 += 167;
        c167 += 168;
        c168 += 169;
        c169 += 170;
        c170 += 171;
        c171 += 172;
        c172 += 173;
        c173 += 174;
        c174 += 175;
        c175 += 176;
        c176 += 177;
        c177 += 178;
        c178 += 179;
        c179 += 180;
        c180 += 181;
        c181 += 182;
        c182 += 183;
        c183 += 184;
        c184 += 185;
        c185 += 186;
        c186 += 187;
        c187 += 188;
        c188 += 189;
        c189 += 190;
        c190 += 191;
        c191 += 192;
        c192 += 193;
        c193 += 194;
        c194 += 195;
        c195 += 196;
        c196 += 197;
        c197 += 198;
        c198 += 199;
        c199 += 200;
        c200 += 201;
        c201 += 202;
        c202 += 203;
        c203 += 204;
        c204 += 205;
        c205 += 206;
        c206 += 207;
        c207 += 208;
        c208 += 209;
        c209 += 210;
        c210 += 211;
        c211 += 212;
        c212 += 213;
        c213 += 214;
        c214 += 215;
        c215 += 216;
        c216 += 217;
        c217 += 218;
        c218 += 219;
        c219 += 220;
        c220 += 221;
        c221 += 222;
        c222 += 223;
        c223 += 224;
        c224 += 225;
        c225 += 226;
        c226 += 227;
        c227 += 228;
        c228 += 229;
        c229 += 230;
        c230 += 231;
        c231 += 232;
        c232 += 233;
        c233 += 234;
        c234 += 235;
        c235 += 236;
        c236 += 237;
        c237 += 238;
        c238 += 239;
        c239 += 240;
        c240 += 241;
        c241 += 242;
        c242 += 243;
        c243 += 244;
        c244 += 245;
        c245 += 246;
        c246 += 247;
        c247 += 248;
        c248 += 249;
        c249 += 250;
        c250 += 251;
        c251 += 252;
        c252 += 253;
        c253 += 254;
        c254 += 255;
        c255 += 256;
        c256 += 257;
        c257 += 258;
        c258 += 259;
        c259 += 260;
    }
    return
          c0 * c259
        + c1 * c258
        + c2 * c257
        + c3 * c256
        + c4 * c255
        + c5 * c254
        + c6 * c253
        + c7 * c252
        + c8 * c251
        + c9 * c250
        + c10 * c249
        + c11 * c248
        + c12 * c247
        + c13 * c246
        + c14 * c245
        + c15 * c244
        + c16 * c243
        + c17 * c242
        + c18 * c241
        + c19 * c240
        + c20 * c239
        + c21 * c238
        + c22 * c237
        + c23 * c236
        + c24 * c235
        + c25 * c234
        + c26 * c233
        + c27 * c232
        + c28 * c231
        + c29 * c230
        + c30 * c229
        + c31 * c228
        + c32 * c227
        + c33 * c226
        + c34 * c225
        + c35 * c224
        + c36 * c223
        + c37 * c222
        + c38 * c221
        + c39 * c220
        + c40 * c219
        + c41 * c218
        + c42 * c217
        + c43 * c216
        + c44 * c215
        + c45 * c214
        + c46 * c213
        + c47 * c212
        + c48 * c211
        + c49 * c210
        + c50 * c209
        + c51 * c208
        + c52 * c207
        + c53 * c206
        + c54 * c205
        + c55 * c204
        + c56 * c203
        + c57 * c202
        + c58 * c201
        + c59 * c200
        + c60 * c199
        + c61 * c198
        + c62 * c197
        + c63 * c196
        + c64 * c195
        + c65 * c194
        + c66 * c193
        + c67 * c192
        + c68 * c191
        + c69 * c190
        + c70 * c189
        + c71 * c188
        + c72 * c187
        + c73 * c186
        + c74 * c185
        + c75 * c184
        + c76 * c183
        + c77 * c182
        + c78 * c181
        + c79 * c180
        + c80 * c179
        + c81 * c178
        + c82 * c177
        + c83 * c176
        + c84 * c175
        + c85 * c174
        + c86 * c173
        + c87 * c172
        + c88 * c171
        + c89 * c170
        + c90 * c169
        + c91 * c168
        + c92 * c167
        + c93 * c166
        + c94 * c165
        + c95 * c164
        + c96 * c163
        + c97 * c162
        + c98 * c161
        + c99 * c160
        + c100 * c159
        + c101 * c158
        + c102 * c157
        + c103 * c156
        + c104 * c155
        + c105 * c154
        + c106 * c153
        + c107 * c152
        + c108 * c151
        + c109 * c150
        + c110 * c149
        + c111 * c148
        + c112 * c147
        + c113 * c146
        + c114 * c145
        + c115 * c144
        + c116 * c143
        + c117 * c142
        + c118 * c141
        + c119 * c140
        + c120 * c139
        + c121 * c138
        + c122 * c137
        + c123 * c136
        + c124 * c135
        + c125 * c134
        + c126 * c133
        + c127 * c132
        + c128 * c131
        + c129 * c130
        + c130 * c129
        + c131 * c128
        + c132 * c127
        + c133 * c126
        + c134 * c125
        + c135 * c124
        + c136 * c123
        + c137 * c122
        + c138 * c121
        + c139 * c120
        + c140 * c119
        + c141 * c118
        + c142 * c117
        + c143 * c116
        + c144 * c115
        + c145 * c114
        + c146 * c113
        + c147 * c112
        + c148 * c111
        + c149 * c110
        + c150 * c109
        + c151 * c108
        + c152 * c107
        + c153 * c106
        + c154 * c105
        + c155 * c104
        + c156 * c103
        + c157 * c102
        + c158 * c101
        + c159 * c100
        + c160 * c99
        + c161 * c98
        + c162 * c97
        + c163 * c96
        + c164 * c95
        + c165 * c94
        + c166 * c93
        + c167 * c92
        + c168 * c91
        + c169 * c90
        + c170 * c89
        + c171 * c88
        + c172 * c87
        + c173 * c86
        + c174 * c85
        + c175 * c84
        + c176 * c83
        + c177 * c82
        + c178 * c81
        + c179 * c80
        + c180 * c79
        + c181 * c78
        + c182 * c77
        + c183 * c76
        + c184 * c75
        + c185 * c74
        + c186 * c73
        + c187 * c72
        + c188 * c71
        + c189 * c70
        + c190 * c69
        + c191 * c68
        + c192 * c67
        + c193 * c66
        + c194 * c65
        + c195 * c64
        + c196 * c63
        + c197 * c62
        + c198 * c61
        + c199 * c60
        + c200 * c59
        + c201 * c58
        + c202 * c57
        + c203 * c56
        + c204 * c55
        + c205 * c54
        + c206 * c53
        + c207 * c52
        + c208 * c51
        + c209 * c50
        + c210 * c49
        + c211 * c48
        + c212 * c47
        + c213 * c46
        + c214 * c45
        + c215 * c44
        + c216 * c43
        + c217 * c42
        + c218 * c41
        + c219 * c40
        + c220 * c39
        + c221 * c38
        + c222 * c37
        + c223 * c36
        + c224 * c35
        + c225 * c34
        + c226 * c33
        + c227 * c32
        + c228 * c31
        + c229 * c30
        + c230 * c29
        + c231 * c28
        + c232 * c27
        + c233 * c26
        + c234 * c25
        + c235 * c24
        + c236 * c23
        + c237 * c22
        + c238 * c21
        + c239 * c20
        + c240 * c19
        + c241 * c18
        + c242 * c17
        + c243 * c16
        + c244 * c15
        + c245 * c14
        + c246 * c13
        + c247 * c12
        + c248 * c11
        + c249 * c10
        + c250 * c9
        + c251 * c8
        + c252 * c7
        + c253 * c6
        + c254 * c5
        + c255 * c4
        + c256 * c3
        + c257 * c2
        + c258 * c1
        + c259 * c0;
}
