import { b as styles_default, a as stateRenderer_v3_unified_default, s as stateDiagram_default, S as StateDB } from "./chunk-AQP2D5EJ-DMcbx5i-.js";
import { a as __name } from "./mermaid.core-Mvl73bOl.js";
import "./chunk-55IACEB6-hmD_bGSN.js";
import "./chunk-2J33WTMH-2iCDyVmR.js";
import "./index-B-PE276Q.js";
import "./step-DgzMt9a6.js";
var diagram = {
  parser: stateDiagram_default,
  get db() {
    return new StateDB(2);
  },
  renderer: stateRenderer_v3_unified_default,
  styles: styles_default,
  init: /* @__PURE__ */ __name((cnf) => {
    if (!cnf.state) {
      cnf.state = {};
    }
    cnf.state.arrowMarkerAbsolute = cnf.arrowMarkerAbsolute;
  }, "init")
};
export {
  diagram
};
