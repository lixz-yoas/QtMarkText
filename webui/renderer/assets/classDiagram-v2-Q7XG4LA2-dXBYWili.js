import { s as styles_default, a as classRenderer_v3_unified_default, c as classDiagram_default, C as ClassDB } from "./chunk-727SXJPM-DGnfyk4H.js";
import { a as __name } from "./mermaid.core-Mvl73bOl.js";
import "./chunk-FMBD7UC4-C0Cx7V3O.js";
import "./chunk-ND2GUHAM-DY1UjY22.js";
import "./chunk-55IACEB6-hmD_bGSN.js";
import "./chunk-2J33WTMH-2iCDyVmR.js";
import "./index-B-PE276Q.js";
import "./step-DgzMt9a6.js";
var diagram = {
  parser: classDiagram_default,
  get db() {
    return new ClassDB();
  },
  renderer: classRenderer_v3_unified_default,
  styles: styles_default,
  init: /* @__PURE__ */ __name((cnf) => {
    if (!cnf.class) {
      cnf.class = {};
    }
    cnf.class.arrowMarkerAbsolute = cnf.arrowMarkerAbsolute;
  }, "init")
};
export {
  diagram
};
