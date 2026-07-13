import { render } from 'preact';
import { createCepBridge } from '../host/cepBridge';
import { createCepFrameFileStore } from '../host/cepFrameFileIO';
import { createRenderFrameSource } from '../host/renderFrameSource';
import { App } from './App';
import './styles.css';

const bridge = createCepBridge();
// The v1 FrameSource: render-to-file through the bridge, read back via cep.fs.
const frameSource = createRenderFrameSource(bridge, createCepFrameFileStore());

render(<App bridge={bridge} frameSource={frameSource} />, document.getElementById('root')!);
