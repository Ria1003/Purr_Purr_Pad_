const fs = require('fs');
const f = 'firebase.json';
const j = JSON.parse(fs.readFileSync(f, 'utf8'));
j.functions ||= {};
j.functions.source = 'functions';
j.functions.runtime = 'nodejs20';
j.functions.predeploy = ['npx -y tsc -p "$RESOURCE_DIR/tsconfig.json"'];
fs.writeFileSync(f, JSON.stringify(j, null, 2));
console.log('Patched', f);
