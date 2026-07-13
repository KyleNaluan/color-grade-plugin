import { render } from 'preact';
import { createCepBridge } from '../host/cepBridge';
import { App } from './App';
import './styles.css';

render(<App bridge={createCepBridge()} />, document.getElementById('root')!);
