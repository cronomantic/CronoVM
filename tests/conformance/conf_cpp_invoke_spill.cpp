/* conf_cpp_invoke_spill.cpp - a SPILLED scalar `invoke` result must reach its frame slot.
 *
 * A throwing call inside an EH scope lowers to an `invoke` (a terminator that branches to
 * its normal/unwind successors). The translator persisted a spilled scalar result to its
 * value-spill slot via the GENERIC post-instruction store - but for a terminator that store
 * is emitted AFTER the block's branch, so it is unreachable. The spilled result was therefore
 * never written to its slot and reloaded as garbage. (Surfaced by Exult
 * Image_window::static_init, where use_facet<ctype<char>>() - an invoke returning a facet
 * pointer kept live under heavy register pressure - came back null and the next virtual call
 * dispatched through a garbage vtable. Fix: store the spilled result on the normal path,
 * before the branch.)
 *
 * This keeps 240 invoke results (from a throwing helper) live SIMULTANEOUSLY until one
 * weighted sum, which exceeds the register file so the allocator MUST spill many of them.
 * With the bug the spilled ones read back garbage and the checksum is wrong; the weights
 * make every value matter. The throw is never taken, so the catch is dormant - it exists
 * only to make the calls `invoke`s. Accumulated in unsigned (defined overflow) so VM-32
 * and native-64 agree. */
#include <cstdint>

struct Exc { int32_t code; };

__attribute__((noinline)) static int32_t thrower(int32_t x) {
    if (x == (int32_t)0x7fffffff) throw Exc{42};   /* makes the call site an invoke */
    return x * 3 + 1;
}

extern "C" int conf_main(void) {
    uint32_t acc = 0;
    try {
        int32_t a0 = thrower(1);
        int32_t a1 = thrower(2);
        int32_t a2 = thrower(3);
        int32_t a3 = thrower(4);
        int32_t a4 = thrower(5);
        int32_t a5 = thrower(6);
        int32_t a6 = thrower(7);
        int32_t a7 = thrower(8);
        int32_t a8 = thrower(9);
        int32_t a9 = thrower(10);
        int32_t a10 = thrower(11);
        int32_t a11 = thrower(12);
        int32_t a12 = thrower(13);
        int32_t a13 = thrower(14);
        int32_t a14 = thrower(15);
        int32_t a15 = thrower(16);
        int32_t a16 = thrower(17);
        int32_t a17 = thrower(18);
        int32_t a18 = thrower(19);
        int32_t a19 = thrower(20);
        int32_t a20 = thrower(21);
        int32_t a21 = thrower(22);
        int32_t a22 = thrower(23);
        int32_t a23 = thrower(24);
        int32_t a24 = thrower(25);
        int32_t a25 = thrower(26);
        int32_t a26 = thrower(27);
        int32_t a27 = thrower(28);
        int32_t a28 = thrower(29);
        int32_t a29 = thrower(30);
        int32_t a30 = thrower(31);
        int32_t a31 = thrower(32);
        int32_t a32 = thrower(33);
        int32_t a33 = thrower(34);
        int32_t a34 = thrower(35);
        int32_t a35 = thrower(36);
        int32_t a36 = thrower(37);
        int32_t a37 = thrower(38);
        int32_t a38 = thrower(39);
        int32_t a39 = thrower(40);
        int32_t a40 = thrower(41);
        int32_t a41 = thrower(42);
        int32_t a42 = thrower(43);
        int32_t a43 = thrower(44);
        int32_t a44 = thrower(45);
        int32_t a45 = thrower(46);
        int32_t a46 = thrower(47);
        int32_t a47 = thrower(48);
        int32_t a48 = thrower(49);
        int32_t a49 = thrower(50);
        int32_t a50 = thrower(51);
        int32_t a51 = thrower(52);
        int32_t a52 = thrower(53);
        int32_t a53 = thrower(54);
        int32_t a54 = thrower(55);
        int32_t a55 = thrower(56);
        int32_t a56 = thrower(57);
        int32_t a57 = thrower(58);
        int32_t a58 = thrower(59);
        int32_t a59 = thrower(60);
        int32_t a60 = thrower(61);
        int32_t a61 = thrower(62);
        int32_t a62 = thrower(63);
        int32_t a63 = thrower(64);
        int32_t a64 = thrower(65);
        int32_t a65 = thrower(66);
        int32_t a66 = thrower(67);
        int32_t a67 = thrower(68);
        int32_t a68 = thrower(69);
        int32_t a69 = thrower(70);
        int32_t a70 = thrower(71);
        int32_t a71 = thrower(72);
        int32_t a72 = thrower(73);
        int32_t a73 = thrower(74);
        int32_t a74 = thrower(75);
        int32_t a75 = thrower(76);
        int32_t a76 = thrower(77);
        int32_t a77 = thrower(78);
        int32_t a78 = thrower(79);
        int32_t a79 = thrower(80);
        int32_t a80 = thrower(81);
        int32_t a81 = thrower(82);
        int32_t a82 = thrower(83);
        int32_t a83 = thrower(84);
        int32_t a84 = thrower(85);
        int32_t a85 = thrower(86);
        int32_t a86 = thrower(87);
        int32_t a87 = thrower(88);
        int32_t a88 = thrower(89);
        int32_t a89 = thrower(90);
        int32_t a90 = thrower(91);
        int32_t a91 = thrower(92);
        int32_t a92 = thrower(93);
        int32_t a93 = thrower(94);
        int32_t a94 = thrower(95);
        int32_t a95 = thrower(96);
        int32_t a96 = thrower(97);
        int32_t a97 = thrower(98);
        int32_t a98 = thrower(99);
        int32_t a99 = thrower(100);
        int32_t a100 = thrower(101);
        int32_t a101 = thrower(102);
        int32_t a102 = thrower(103);
        int32_t a103 = thrower(104);
        int32_t a104 = thrower(105);
        int32_t a105 = thrower(106);
        int32_t a106 = thrower(107);
        int32_t a107 = thrower(108);
        int32_t a108 = thrower(109);
        int32_t a109 = thrower(110);
        int32_t a110 = thrower(111);
        int32_t a111 = thrower(112);
        int32_t a112 = thrower(113);
        int32_t a113 = thrower(114);
        int32_t a114 = thrower(115);
        int32_t a115 = thrower(116);
        int32_t a116 = thrower(117);
        int32_t a117 = thrower(118);
        int32_t a118 = thrower(119);
        int32_t a119 = thrower(120);
        int32_t a120 = thrower(121);
        int32_t a121 = thrower(122);
        int32_t a122 = thrower(123);
        int32_t a123 = thrower(124);
        int32_t a124 = thrower(125);
        int32_t a125 = thrower(126);
        int32_t a126 = thrower(127);
        int32_t a127 = thrower(128);
        int32_t a128 = thrower(129);
        int32_t a129 = thrower(130);
        int32_t a130 = thrower(131);
        int32_t a131 = thrower(132);
        int32_t a132 = thrower(133);
        int32_t a133 = thrower(134);
        int32_t a134 = thrower(135);
        int32_t a135 = thrower(136);
        int32_t a136 = thrower(137);
        int32_t a137 = thrower(138);
        int32_t a138 = thrower(139);
        int32_t a139 = thrower(140);
        int32_t a140 = thrower(141);
        int32_t a141 = thrower(142);
        int32_t a142 = thrower(143);
        int32_t a143 = thrower(144);
        int32_t a144 = thrower(145);
        int32_t a145 = thrower(146);
        int32_t a146 = thrower(147);
        int32_t a147 = thrower(148);
        int32_t a148 = thrower(149);
        int32_t a149 = thrower(150);
        int32_t a150 = thrower(151);
        int32_t a151 = thrower(152);
        int32_t a152 = thrower(153);
        int32_t a153 = thrower(154);
        int32_t a154 = thrower(155);
        int32_t a155 = thrower(156);
        int32_t a156 = thrower(157);
        int32_t a157 = thrower(158);
        int32_t a158 = thrower(159);
        int32_t a159 = thrower(160);
        int32_t a160 = thrower(161);
        int32_t a161 = thrower(162);
        int32_t a162 = thrower(163);
        int32_t a163 = thrower(164);
        int32_t a164 = thrower(165);
        int32_t a165 = thrower(166);
        int32_t a166 = thrower(167);
        int32_t a167 = thrower(168);
        int32_t a168 = thrower(169);
        int32_t a169 = thrower(170);
        int32_t a170 = thrower(171);
        int32_t a171 = thrower(172);
        int32_t a172 = thrower(173);
        int32_t a173 = thrower(174);
        int32_t a174 = thrower(175);
        int32_t a175 = thrower(176);
        int32_t a176 = thrower(177);
        int32_t a177 = thrower(178);
        int32_t a178 = thrower(179);
        int32_t a179 = thrower(180);
        int32_t a180 = thrower(181);
        int32_t a181 = thrower(182);
        int32_t a182 = thrower(183);
        int32_t a183 = thrower(184);
        int32_t a184 = thrower(185);
        int32_t a185 = thrower(186);
        int32_t a186 = thrower(187);
        int32_t a187 = thrower(188);
        int32_t a188 = thrower(189);
        int32_t a189 = thrower(190);
        int32_t a190 = thrower(191);
        int32_t a191 = thrower(192);
        int32_t a192 = thrower(193);
        int32_t a193 = thrower(194);
        int32_t a194 = thrower(195);
        int32_t a195 = thrower(196);
        int32_t a196 = thrower(197);
        int32_t a197 = thrower(198);
        int32_t a198 = thrower(199);
        int32_t a199 = thrower(200);
        int32_t a200 = thrower(201);
        int32_t a201 = thrower(202);
        int32_t a202 = thrower(203);
        int32_t a203 = thrower(204);
        int32_t a204 = thrower(205);
        int32_t a205 = thrower(206);
        int32_t a206 = thrower(207);
        int32_t a207 = thrower(208);
        int32_t a208 = thrower(209);
        int32_t a209 = thrower(210);
        int32_t a210 = thrower(211);
        int32_t a211 = thrower(212);
        int32_t a212 = thrower(213);
        int32_t a213 = thrower(214);
        int32_t a214 = thrower(215);
        int32_t a215 = thrower(216);
        int32_t a216 = thrower(217);
        int32_t a217 = thrower(218);
        int32_t a218 = thrower(219);
        int32_t a219 = thrower(220);
        int32_t a220 = thrower(221);
        int32_t a221 = thrower(222);
        int32_t a222 = thrower(223);
        int32_t a223 = thrower(224);
        int32_t a224 = thrower(225);
        int32_t a225 = thrower(226);
        int32_t a226 = thrower(227);
        int32_t a227 = thrower(228);
        int32_t a228 = thrower(229);
        int32_t a229 = thrower(230);
        int32_t a230 = thrower(231);
        int32_t a231 = thrower(232);
        int32_t a232 = thrower(233);
        int32_t a233 = thrower(234);
        int32_t a234 = thrower(235);
        int32_t a235 = thrower(236);
        int32_t a236 = thrower(237);
        int32_t a237 = thrower(238);
        int32_t a238 = thrower(239);
        int32_t a239 = thrower(240);
        /* every a_i live until here -> forced spilling */
        acc = (uint32_t)a0 * 1u + (uint32_t)a1 * 2u + (uint32_t)a2 * 3u + (uint32_t)a3 * 4u + (uint32_t)a4 * 5u + (uint32_t)a5 * 6u + (uint32_t)a6 * 7u + (uint32_t)a7 * 8u + (uint32_t)a8 * 9u + (uint32_t)a9 * 10u + (uint32_t)a10 * 11u + (uint32_t)a11 * 12u + (uint32_t)a12 * 13u + (uint32_t)a13 * 14u + (uint32_t)a14 * 15u + (uint32_t)a15 * 16u + (uint32_t)a16 * 17u + (uint32_t)a17 * 18u + (uint32_t)a18 * 19u + (uint32_t)a19 * 20u + (uint32_t)a20 * 21u + (uint32_t)a21 * 22u + (uint32_t)a22 * 23u + (uint32_t)a23 * 24u + (uint32_t)a24 * 25u + (uint32_t)a25 * 26u + (uint32_t)a26 * 27u + (uint32_t)a27 * 28u + (uint32_t)a28 * 29u + (uint32_t)a29 * 30u + (uint32_t)a30 * 31u + (uint32_t)a31 * 32u + (uint32_t)a32 * 33u + (uint32_t)a33 * 34u + (uint32_t)a34 * 35u + (uint32_t)a35 * 36u + (uint32_t)a36 * 37u + (uint32_t)a37 * 38u + (uint32_t)a38 * 39u + (uint32_t)a39 * 40u + (uint32_t)a40 * 41u + (uint32_t)a41 * 42u + (uint32_t)a42 * 43u + (uint32_t)a43 * 44u + (uint32_t)a44 * 45u + (uint32_t)a45 * 46u + (uint32_t)a46 * 47u + (uint32_t)a47 * 48u + (uint32_t)a48 * 49u + (uint32_t)a49 * 50u + (uint32_t)a50 * 51u + (uint32_t)a51 * 52u + (uint32_t)a52 * 53u + (uint32_t)a53 * 54u + (uint32_t)a54 * 55u + (uint32_t)a55 * 56u + (uint32_t)a56 * 57u + (uint32_t)a57 * 58u + (uint32_t)a58 * 59u + (uint32_t)a59 * 60u + (uint32_t)a60 * 61u + (uint32_t)a61 * 62u + (uint32_t)a62 * 63u + (uint32_t)a63 * 64u + (uint32_t)a64 * 65u + (uint32_t)a65 * 66u + (uint32_t)a66 * 67u + (uint32_t)a67 * 68u + (uint32_t)a68 * 69u + (uint32_t)a69 * 70u + (uint32_t)a70 * 71u + (uint32_t)a71 * 72u + (uint32_t)a72 * 73u + (uint32_t)a73 * 74u + (uint32_t)a74 * 75u + (uint32_t)a75 * 76u + (uint32_t)a76 * 77u + (uint32_t)a77 * 78u + (uint32_t)a78 * 79u + (uint32_t)a79 * 80u + (uint32_t)a80 * 81u + (uint32_t)a81 * 82u + (uint32_t)a82 * 83u + (uint32_t)a83 * 84u + (uint32_t)a84 * 85u + (uint32_t)a85 * 86u + (uint32_t)a86 * 87u + (uint32_t)a87 * 88u + (uint32_t)a88 * 89u + (uint32_t)a89 * 90u + (uint32_t)a90 * 91u + (uint32_t)a91 * 92u + (uint32_t)a92 * 93u + (uint32_t)a93 * 94u + (uint32_t)a94 * 95u + (uint32_t)a95 * 96u + (uint32_t)a96 * 97u + (uint32_t)a97 * 1u + (uint32_t)a98 * 2u + (uint32_t)a99 * 3u + (uint32_t)a100 * 4u + (uint32_t)a101 * 5u + (uint32_t)a102 * 6u + (uint32_t)a103 * 7u + (uint32_t)a104 * 8u + (uint32_t)a105 * 9u + (uint32_t)a106 * 10u + (uint32_t)a107 * 11u + (uint32_t)a108 * 12u + (uint32_t)a109 * 13u + (uint32_t)a110 * 14u + (uint32_t)a111 * 15u + (uint32_t)a112 * 16u + (uint32_t)a113 * 17u + (uint32_t)a114 * 18u + (uint32_t)a115 * 19u + (uint32_t)a116 * 20u + (uint32_t)a117 * 21u + (uint32_t)a118 * 22u + (uint32_t)a119 * 23u + (uint32_t)a120 * 24u + (uint32_t)a121 * 25u + (uint32_t)a122 * 26u + (uint32_t)a123 * 27u + (uint32_t)a124 * 28u + (uint32_t)a125 * 29u + (uint32_t)a126 * 30u + (uint32_t)a127 * 31u + (uint32_t)a128 * 32u + (uint32_t)a129 * 33u + (uint32_t)a130 * 34u + (uint32_t)a131 * 35u + (uint32_t)a132 * 36u + (uint32_t)a133 * 37u + (uint32_t)a134 * 38u + (uint32_t)a135 * 39u + (uint32_t)a136 * 40u + (uint32_t)a137 * 41u + (uint32_t)a138 * 42u + (uint32_t)a139 * 43u + (uint32_t)a140 * 44u + (uint32_t)a141 * 45u + (uint32_t)a142 * 46u + (uint32_t)a143 * 47u + (uint32_t)a144 * 48u + (uint32_t)a145 * 49u + (uint32_t)a146 * 50u + (uint32_t)a147 * 51u + (uint32_t)a148 * 52u + (uint32_t)a149 * 53u + (uint32_t)a150 * 54u + (uint32_t)a151 * 55u + (uint32_t)a152 * 56u + (uint32_t)a153 * 57u + (uint32_t)a154 * 58u + (uint32_t)a155 * 59u + (uint32_t)a156 * 60u + (uint32_t)a157 * 61u + (uint32_t)a158 * 62u + (uint32_t)a159 * 63u + (uint32_t)a160 * 64u + (uint32_t)a161 * 65u + (uint32_t)a162 * 66u + (uint32_t)a163 * 67u + (uint32_t)a164 * 68u + (uint32_t)a165 * 69u + (uint32_t)a166 * 70u + (uint32_t)a167 * 71u + (uint32_t)a168 * 72u + (uint32_t)a169 * 73u + (uint32_t)a170 * 74u + (uint32_t)a171 * 75u + (uint32_t)a172 * 76u + (uint32_t)a173 * 77u + (uint32_t)a174 * 78u + (uint32_t)a175 * 79u + (uint32_t)a176 * 80u + (uint32_t)a177 * 81u + (uint32_t)a178 * 82u + (uint32_t)a179 * 83u + (uint32_t)a180 * 84u + (uint32_t)a181 * 85u + (uint32_t)a182 * 86u + (uint32_t)a183 * 87u + (uint32_t)a184 * 88u + (uint32_t)a185 * 89u + (uint32_t)a186 * 90u + (uint32_t)a187 * 91u + (uint32_t)a188 * 92u + (uint32_t)a189 * 93u + (uint32_t)a190 * 94u + (uint32_t)a191 * 95u + (uint32_t)a192 * 96u + (uint32_t)a193 * 97u + (uint32_t)a194 * 1u + (uint32_t)a195 * 2u + (uint32_t)a196 * 3u + (uint32_t)a197 * 4u + (uint32_t)a198 * 5u + (uint32_t)a199 * 6u + (uint32_t)a200 * 7u + (uint32_t)a201 * 8u + (uint32_t)a202 * 9u + (uint32_t)a203 * 10u + (uint32_t)a204 * 11u + (uint32_t)a205 * 12u + (uint32_t)a206 * 13u + (uint32_t)a207 * 14u + (uint32_t)a208 * 15u + (uint32_t)a209 * 16u + (uint32_t)a210 * 17u + (uint32_t)a211 * 18u + (uint32_t)a212 * 19u + (uint32_t)a213 * 20u + (uint32_t)a214 * 21u + (uint32_t)a215 * 22u + (uint32_t)a216 * 23u + (uint32_t)a217 * 24u + (uint32_t)a218 * 25u + (uint32_t)a219 * 26u + (uint32_t)a220 * 27u + (uint32_t)a221 * 28u + (uint32_t)a222 * 29u + (uint32_t)a223 * 30u + (uint32_t)a224 * 31u + (uint32_t)a225 * 32u + (uint32_t)a226 * 33u + (uint32_t)a227 * 34u + (uint32_t)a228 * 35u + (uint32_t)a229 * 36u + (uint32_t)a230 * 37u + (uint32_t)a231 * 38u + (uint32_t)a232 * 39u + (uint32_t)a233 * 40u + (uint32_t)a234 * 41u + (uint32_t)a235 * 42u + (uint32_t)a236 * 43u + (uint32_t)a237 * 44u + (uint32_t)a238 * 45u + (uint32_t)a239 * 46u;
    } catch (const Exc& e) {
        acc = (uint32_t)(-e.code);
    }
    return (int)acc;
}
