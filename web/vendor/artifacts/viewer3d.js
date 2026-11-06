/* Built-in 3D viewer for model3d artifacts. Runs inside the sandboxed artifact page: loads the STL or GLB named by
   <body data-model="..."> from the same folder, frames it, and offers orbit controls. Sets window.__ready (and
   <html data-ready="1">) after the first frame is drawn, or <html data-ready="error"> with a message on screen. */
import * as THREE from "three";
import { OrbitControls } from "three/addons/OrbitControls.js";
import { STLLoader } from "three/addons/STLLoader.js";
import { GLTFLoader } from "three/addons/GLTFLoader.js";

const dark = window.matchMedia && matchMedia("(prefers-color-scheme: dark)").matches;
document.documentElement.style.cssText = "height:100%";
document.body.style.cssText = "margin:0;height:100%;overflow:hidden;background:" + (dark ? "#17181b" : "#eef0f3") +
  ";font:13px system-ui,sans-serif;color:" + (dark ? "#aab" : "#556");

function note(text, isError) {
  let n = document.getElementById("note");
  if (!n) {
    n = document.createElement("div");
    n.id = "note";
    n.style.cssText = "position:fixed;left:12px;bottom:10px;pointer-events:none;opacity:.8";
    document.body.appendChild(n);
  }
  n.textContent = text;
  if (isError) document.documentElement.dataset.ready = "error";
}

async function main() {
  const path = document.body.dataset.model;
  if (!path) throw new Error("no model named");
  const res = await fetch(path);
  if (!res.ok) throw new Error("cannot load " + path + " (" + res.status + ")");
  const buf = await res.arrayBuffer();
  const isGlb = /\.glb$/i.test(path);

  const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, preserveDrawingBuffer: true });
  renderer.setPixelRatio(Math.min(devicePixelRatio || 1, 2));
  renderer.setClearColor(dark ? 0x17181b : 0xeef0f3);
  document.body.appendChild(renderer.domElement);
  renderer.domElement.style.cssText = "display:block;width:100%;height:100%;touch-action:none";

  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(35, 1, 0.01, 1e6);
  let root;
  if (isGlb) {
    const gltf = await new Promise((ok, bad) => new GLTFLoader().parse(buf, "", ok, bad));
    root = gltf.scene;
    camera.up.set(0, 1, 0);
  } else {
    const geo = new STLLoader().parse(buf);
    geo.computeVertexNormals();
    const mat = new THREE.MeshStandardMaterial({ color: 0x9fb0c6, roughness: 0.55, metalness: 0.1, flatShading: true, side: THREE.DoubleSide });
    root = new THREE.Group();
    root.add(new THREE.Mesh(geo, mat));
    root.add(new THREE.LineSegments(new THREE.EdgesGeometry(geo, 35), new THREE.LineBasicMaterial({ color: dark ? 0x0b0c0e : 0x2a3140 })));
    camera.up.set(0, 0, 1);   // STL is Z up
  }
  scene.add(root);
  scene.add(new THREE.HemisphereLight(0xffffff, dark ? 0x30343c : 0x9aa3b2, 1.1));
  const key = new THREE.DirectionalLight(0xffffff, 2.2);
  const fill = new THREE.DirectionalLight(0xffffff, 0.7);
  scene.add(key, fill);

  const box = new THREE.Box3().setFromObject(root);
  const center = box.getCenter(new THREE.Vector3());
  const radius = Math.max(box.getSize(new THREE.Vector3()).length() / 2, 1e-6);
  const controls = new OrbitControls(camera, renderer.domElement);
  controls.target.copy(center);
  controls.enableDamping = true;
  const dir = isGlb ? new THREE.Vector3(0.7, 0.6, 0.9) : new THREE.Vector3(0.62, -0.78, 0.72);
  const asp = (innerWidth || 640) / (innerHeight || 480);   // a tall window needs more distance to keep the width in view
  camera.position.copy(center).addScaledVector(dir.normalize(), radius * 3.2 * Math.max(1, 1 / asp));
  camera.near = radius / 100;
  camera.far = radius * 100;
  camera.updateProjectionMatrix();
  controls.update();

  function size() {
    const w = innerWidth || 640, h = innerHeight || 480;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  function draw() {
    key.position.copy(camera.position).add(new THREE.Vector3(-1, 1, 0.5).multiplyScalar(radius));
    fill.position.copy(camera.position).add(new THREE.Vector3(1, -0.3, 0.4).multiplyScalar(radius));
    renderer.render(scene, camera);
  }
  let queued = false;
  function want() { if (!queued) { queued = true; requestAnimationFrame(() => { queued = false; controls.update(); draw(); }); } }
  controls.addEventListener("change", want);
  addEventListener("resize", () => { size(); want(); });
  size();
  draw();
  note("drag to orbit, scroll to zoom, right drag to pan");
  document.documentElement.dataset.ready = "1";
  window.__ready = true;
}

main().catch((e) => { note("Could not show this model: " + (e && e.message ? e.message : e), true); window.__ready = false; });
