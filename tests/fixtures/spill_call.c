/* Register-pressure stress fixture (value spilling ACROSS a CALL).
 *
 * Exercises the interaction between value-spilling and the existing
 * caller-save-spill-across-CALL machinery, plus a call with many spilled
 * arguments.
 *
 * 240 carriers c0..cN are derived from a zero-init global `g` (opaque) and
 * `n`, so all are simultaneously live. A noinline helper `sink` is called
 * with EIGHT carriers as register args plus the running checksum; because
 * the carriers are live across the call AND many of them are value-spilled,
 * this hits both the caller-save path (for the few still in registers) and
 * the value-spill path (most are already in memory, so caller-save must
 * skip them). The call's spilled args are loaded straight from their slots.
 *
 * `sink(a..h, acc) = acc + a-b+c-d+e-f+g-h`, noinline so the call survives.
 * Final result threads the checksum through one sink call and then sums the
 * palindrome products of the carriers (forcing them all live to the end).
 * Exact i32 wraparound; expected values checked in the e2e tests.
 *
 * Generated file; see CMakeLists.txt.
 */

int g[240];

__attribute__((noinline))
static int sink(int a,int b,int c,int d,int e,int f,int gg,int h,int acc){
    return acc + a - b + c - d + e - f + gg - h;
}

int vm_main(int n) {
    int c0 = g[0] + n * 1;
    int c1 = g[1] + n * 2;
    int c2 = g[2] + n * 3;
    int c3 = g[3] + n * 4;
    int c4 = g[4] + n * 5;
    int c5 = g[5] + n * 6;
    int c6 = g[6] + n * 7;
    int c7 = g[7] + n * 8;
    int c8 = g[8] + n * 9;
    int c9 = g[9] + n * 10;
    int c10 = g[10] + n * 11;
    int c11 = g[11] + n * 12;
    int c12 = g[12] + n * 13;
    int c13 = g[13] + n * 14;
    int c14 = g[14] + n * 15;
    int c15 = g[15] + n * 16;
    int c16 = g[16] + n * 17;
    int c17 = g[17] + n * 18;
    int c18 = g[18] + n * 19;
    int c19 = g[19] + n * 20;
    int c20 = g[20] + n * 21;
    int c21 = g[21] + n * 22;
    int c22 = g[22] + n * 23;
    int c23 = g[23] + n * 24;
    int c24 = g[24] + n * 25;
    int c25 = g[25] + n * 26;
    int c26 = g[26] + n * 27;
    int c27 = g[27] + n * 28;
    int c28 = g[28] + n * 29;
    int c29 = g[29] + n * 30;
    int c30 = g[30] + n * 31;
    int c31 = g[31] + n * 32;
    int c32 = g[32] + n * 33;
    int c33 = g[33] + n * 34;
    int c34 = g[34] + n * 35;
    int c35 = g[35] + n * 36;
    int c36 = g[36] + n * 37;
    int c37 = g[37] + n * 38;
    int c38 = g[38] + n * 39;
    int c39 = g[39] + n * 40;
    int c40 = g[40] + n * 41;
    int c41 = g[41] + n * 42;
    int c42 = g[42] + n * 43;
    int c43 = g[43] + n * 44;
    int c44 = g[44] + n * 45;
    int c45 = g[45] + n * 46;
    int c46 = g[46] + n * 47;
    int c47 = g[47] + n * 48;
    int c48 = g[48] + n * 49;
    int c49 = g[49] + n * 50;
    int c50 = g[50] + n * 51;
    int c51 = g[51] + n * 52;
    int c52 = g[52] + n * 53;
    int c53 = g[53] + n * 54;
    int c54 = g[54] + n * 55;
    int c55 = g[55] + n * 56;
    int c56 = g[56] + n * 57;
    int c57 = g[57] + n * 58;
    int c58 = g[58] + n * 59;
    int c59 = g[59] + n * 60;
    int c60 = g[60] + n * 61;
    int c61 = g[61] + n * 62;
    int c62 = g[62] + n * 63;
    int c63 = g[63] + n * 64;
    int c64 = g[64] + n * 65;
    int c65 = g[65] + n * 66;
    int c66 = g[66] + n * 67;
    int c67 = g[67] + n * 68;
    int c68 = g[68] + n * 69;
    int c69 = g[69] + n * 70;
    int c70 = g[70] + n * 71;
    int c71 = g[71] + n * 72;
    int c72 = g[72] + n * 73;
    int c73 = g[73] + n * 74;
    int c74 = g[74] + n * 75;
    int c75 = g[75] + n * 76;
    int c76 = g[76] + n * 77;
    int c77 = g[77] + n * 78;
    int c78 = g[78] + n * 79;
    int c79 = g[79] + n * 80;
    int c80 = g[80] + n * 81;
    int c81 = g[81] + n * 82;
    int c82 = g[82] + n * 83;
    int c83 = g[83] + n * 84;
    int c84 = g[84] + n * 85;
    int c85 = g[85] + n * 86;
    int c86 = g[86] + n * 87;
    int c87 = g[87] + n * 88;
    int c88 = g[88] + n * 89;
    int c89 = g[89] + n * 90;
    int c90 = g[90] + n * 91;
    int c91 = g[91] + n * 92;
    int c92 = g[92] + n * 93;
    int c93 = g[93] + n * 94;
    int c94 = g[94] + n * 95;
    int c95 = g[95] + n * 96;
    int c96 = g[96] + n * 97;
    int c97 = g[97] + n * 98;
    int c98 = g[98] + n * 99;
    int c99 = g[99] + n * 100;
    int c100 = g[100] + n * 101;
    int c101 = g[101] + n * 102;
    int c102 = g[102] + n * 103;
    int c103 = g[103] + n * 104;
    int c104 = g[104] + n * 105;
    int c105 = g[105] + n * 106;
    int c106 = g[106] + n * 107;
    int c107 = g[107] + n * 108;
    int c108 = g[108] + n * 109;
    int c109 = g[109] + n * 110;
    int c110 = g[110] + n * 111;
    int c111 = g[111] + n * 112;
    int c112 = g[112] + n * 113;
    int c113 = g[113] + n * 114;
    int c114 = g[114] + n * 115;
    int c115 = g[115] + n * 116;
    int c116 = g[116] + n * 117;
    int c117 = g[117] + n * 118;
    int c118 = g[118] + n * 119;
    int c119 = g[119] + n * 120;
    int c120 = g[120] + n * 121;
    int c121 = g[121] + n * 122;
    int c122 = g[122] + n * 123;
    int c123 = g[123] + n * 124;
    int c124 = g[124] + n * 125;
    int c125 = g[125] + n * 126;
    int c126 = g[126] + n * 127;
    int c127 = g[127] + n * 128;
    int c128 = g[128] + n * 129;
    int c129 = g[129] + n * 130;
    int c130 = g[130] + n * 131;
    int c131 = g[131] + n * 132;
    int c132 = g[132] + n * 133;
    int c133 = g[133] + n * 134;
    int c134 = g[134] + n * 135;
    int c135 = g[135] + n * 136;
    int c136 = g[136] + n * 137;
    int c137 = g[137] + n * 138;
    int c138 = g[138] + n * 139;
    int c139 = g[139] + n * 140;
    int c140 = g[140] + n * 141;
    int c141 = g[141] + n * 142;
    int c142 = g[142] + n * 143;
    int c143 = g[143] + n * 144;
    int c144 = g[144] + n * 145;
    int c145 = g[145] + n * 146;
    int c146 = g[146] + n * 147;
    int c147 = g[147] + n * 148;
    int c148 = g[148] + n * 149;
    int c149 = g[149] + n * 150;
    int c150 = g[150] + n * 151;
    int c151 = g[151] + n * 152;
    int c152 = g[152] + n * 153;
    int c153 = g[153] + n * 154;
    int c154 = g[154] + n * 155;
    int c155 = g[155] + n * 156;
    int c156 = g[156] + n * 157;
    int c157 = g[157] + n * 158;
    int c158 = g[158] + n * 159;
    int c159 = g[159] + n * 160;
    int c160 = g[160] + n * 161;
    int c161 = g[161] + n * 162;
    int c162 = g[162] + n * 163;
    int c163 = g[163] + n * 164;
    int c164 = g[164] + n * 165;
    int c165 = g[165] + n * 166;
    int c166 = g[166] + n * 167;
    int c167 = g[167] + n * 168;
    int c168 = g[168] + n * 169;
    int c169 = g[169] + n * 170;
    int c170 = g[170] + n * 171;
    int c171 = g[171] + n * 172;
    int c172 = g[172] + n * 173;
    int c173 = g[173] + n * 174;
    int c174 = g[174] + n * 175;
    int c175 = g[175] + n * 176;
    int c176 = g[176] + n * 177;
    int c177 = g[177] + n * 178;
    int c178 = g[178] + n * 179;
    int c179 = g[179] + n * 180;
    int c180 = g[180] + n * 181;
    int c181 = g[181] + n * 182;
    int c182 = g[182] + n * 183;
    int c183 = g[183] + n * 184;
    int c184 = g[184] + n * 185;
    int c185 = g[185] + n * 186;
    int c186 = g[186] + n * 187;
    int c187 = g[187] + n * 188;
    int c188 = g[188] + n * 189;
    int c189 = g[189] + n * 190;
    int c190 = g[190] + n * 191;
    int c191 = g[191] + n * 192;
    int c192 = g[192] + n * 193;
    int c193 = g[193] + n * 194;
    int c194 = g[194] + n * 195;
    int c195 = g[195] + n * 196;
    int c196 = g[196] + n * 197;
    int c197 = g[197] + n * 198;
    int c198 = g[198] + n * 199;
    int c199 = g[199] + n * 200;
    int c200 = g[200] + n * 201;
    int c201 = g[201] + n * 202;
    int c202 = g[202] + n * 203;
    int c203 = g[203] + n * 204;
    int c204 = g[204] + n * 205;
    int c205 = g[205] + n * 206;
    int c206 = g[206] + n * 207;
    int c207 = g[207] + n * 208;
    int c208 = g[208] + n * 209;
    int c209 = g[209] + n * 210;
    int c210 = g[210] + n * 211;
    int c211 = g[211] + n * 212;
    int c212 = g[212] + n * 213;
    int c213 = g[213] + n * 214;
    int c214 = g[214] + n * 215;
    int c215 = g[215] + n * 216;
    int c216 = g[216] + n * 217;
    int c217 = g[217] + n * 218;
    int c218 = g[218] + n * 219;
    int c219 = g[219] + n * 220;
    int c220 = g[220] + n * 221;
    int c221 = g[221] + n * 222;
    int c222 = g[222] + n * 223;
    int c223 = g[223] + n * 224;
    int c224 = g[224] + n * 225;
    int c225 = g[225] + n * 226;
    int c226 = g[226] + n * 227;
    int c227 = g[227] + n * 228;
    int c228 = g[228] + n * 229;
    int c229 = g[229] + n * 230;
    int c230 = g[230] + n * 231;
    int c231 = g[231] + n * 232;
    int c232 = g[232] + n * 233;
    int c233 = g[233] + n * 234;
    int c234 = g[234] + n * 235;
    int c235 = g[235] + n * 236;
    int c236 = g[236] + n * 237;
    int c237 = g[237] + n * 238;
    int c238 = g[238] + n * 239;
    int c239 = g[239] + n * 240;
    int acc = sink(c0, c1, c2, c3, c4, c5, c6, c7, n);
    return acc +
          c0 * c239
        + c1 * c238
        + c2 * c237
        + c3 * c236
        + c4 * c235
        + c5 * c234
        + c6 * c233
        + c7 * c232
        + c8 * c231
        + c9 * c230
        + c10 * c229
        + c11 * c228
        + c12 * c227
        + c13 * c226
        + c14 * c225
        + c15 * c224
        + c16 * c223
        + c17 * c222
        + c18 * c221
        + c19 * c220
        + c20 * c219
        + c21 * c218
        + c22 * c217
        + c23 * c216
        + c24 * c215
        + c25 * c214
        + c26 * c213
        + c27 * c212
        + c28 * c211
        + c29 * c210
        + c30 * c209
        + c31 * c208
        + c32 * c207
        + c33 * c206
        + c34 * c205
        + c35 * c204
        + c36 * c203
        + c37 * c202
        + c38 * c201
        + c39 * c200
        + c40 * c199
        + c41 * c198
        + c42 * c197
        + c43 * c196
        + c44 * c195
        + c45 * c194
        + c46 * c193
        + c47 * c192
        + c48 * c191
        + c49 * c190
        + c50 * c189
        + c51 * c188
        + c52 * c187
        + c53 * c186
        + c54 * c185
        + c55 * c184
        + c56 * c183
        + c57 * c182
        + c58 * c181
        + c59 * c180
        + c60 * c179
        + c61 * c178
        + c62 * c177
        + c63 * c176
        + c64 * c175
        + c65 * c174
        + c66 * c173
        + c67 * c172
        + c68 * c171
        + c69 * c170
        + c70 * c169
        + c71 * c168
        + c72 * c167
        + c73 * c166
        + c74 * c165
        + c75 * c164
        + c76 * c163
        + c77 * c162
        + c78 * c161
        + c79 * c160
        + c80 * c159
        + c81 * c158
        + c82 * c157
        + c83 * c156
        + c84 * c155
        + c85 * c154
        + c86 * c153
        + c87 * c152
        + c88 * c151
        + c89 * c150
        + c90 * c149
        + c91 * c148
        + c92 * c147
        + c93 * c146
        + c94 * c145
        + c95 * c144
        + c96 * c143
        + c97 * c142
        + c98 * c141
        + c99 * c140
        + c100 * c139
        + c101 * c138
        + c102 * c137
        + c103 * c136
        + c104 * c135
        + c105 * c134
        + c106 * c133
        + c107 * c132
        + c108 * c131
        + c109 * c130
        + c110 * c129
        + c111 * c128
        + c112 * c127
        + c113 * c126
        + c114 * c125
        + c115 * c124
        + c116 * c123
        + c117 * c122
        + c118 * c121
        + c119 * c120
        + c120 * c119
        + c121 * c118
        + c122 * c117
        + c123 * c116
        + c124 * c115
        + c125 * c114
        + c126 * c113
        + c127 * c112
        + c128 * c111
        + c129 * c110
        + c130 * c109
        + c131 * c108
        + c132 * c107
        + c133 * c106
        + c134 * c105
        + c135 * c104
        + c136 * c103
        + c137 * c102
        + c138 * c101
        + c139 * c100
        + c140 * c99
        + c141 * c98
        + c142 * c97
        + c143 * c96
        + c144 * c95
        + c145 * c94
        + c146 * c93
        + c147 * c92
        + c148 * c91
        + c149 * c90
        + c150 * c89
        + c151 * c88
        + c152 * c87
        + c153 * c86
        + c154 * c85
        + c155 * c84
        + c156 * c83
        + c157 * c82
        + c158 * c81
        + c159 * c80
        + c160 * c79
        + c161 * c78
        + c162 * c77
        + c163 * c76
        + c164 * c75
        + c165 * c74
        + c166 * c73
        + c167 * c72
        + c168 * c71
        + c169 * c70
        + c170 * c69
        + c171 * c68
        + c172 * c67
        + c173 * c66
        + c174 * c65
        + c175 * c64
        + c176 * c63
        + c177 * c62
        + c178 * c61
        + c179 * c60
        + c180 * c59
        + c181 * c58
        + c182 * c57
        + c183 * c56
        + c184 * c55
        + c185 * c54
        + c186 * c53
        + c187 * c52
        + c188 * c51
        + c189 * c50
        + c190 * c49
        + c191 * c48
        + c192 * c47
        + c193 * c46
        + c194 * c45
        + c195 * c44
        + c196 * c43
        + c197 * c42
        + c198 * c41
        + c199 * c40
        + c200 * c39
        + c201 * c38
        + c202 * c37
        + c203 * c36
        + c204 * c35
        + c205 * c34
        + c206 * c33
        + c207 * c32
        + c208 * c31
        + c209 * c30
        + c210 * c29
        + c211 * c28
        + c212 * c27
        + c213 * c26
        + c214 * c25
        + c215 * c24
        + c216 * c23
        + c217 * c22
        + c218 * c21
        + c219 * c20
        + c220 * c19
        + c221 * c18
        + c222 * c17
        + c223 * c16
        + c224 * c15
        + c225 * c14
        + c226 * c13
        + c227 * c12
        + c228 * c11
        + c229 * c10
        + c230 * c9
        + c231 * c8
        + c232 * c7
        + c233 * c6
        + c234 * c5
        + c235 * c4
        + c236 * c3
        + c237 * c2
        + c238 * c1
        + c239 * c0;
}
