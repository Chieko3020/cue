import { createRoot } from 'react-dom/client';
import App from './App.jsx';
import './style.css';

// 不用 StrictMode：它会在开发时双跑 effect，导致 EventSource 建两条连接（SSE 会重复）
createRoot(document.getElementById('root')).render(<App />);
