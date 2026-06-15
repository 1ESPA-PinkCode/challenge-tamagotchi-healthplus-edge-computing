// Carrega as variáveis do arquivo .env.
// Isso permite configurar VM, portas e atributos sem alterar o código.
require('dotenv').config();

const express = require('express');
const cors = require('cors');

const app = express();

// Libera o acesso da aplicação por diferentes origens.
app.use(cors());

// Define a pasta public como a pasta dos arquivos do dashboard.
app.use(express.static('public'));

// Configurações principais do dashboard.
// Os valores vêm do .env, mas possuem valores padrão caso não sejam definidos.
const config = {
  vmHost: process.env.VM_HOST || '35.255.8.111',
  historyPort: process.env.HISTORY_PORT || '8666',
  provider: (process.env.HISTORIC_PROVIDER || 'sth').toLowerCase(),
  fiwareService: process.env.FIWARE_SERVICE || 'smart',
  fiwareServicePath: process.env.FIWARE_SERVICE_PATH || '/',
  entityId: process.env.ENTITY_ID || 'chaveiro001',
  entityType: process.env.ENTITY_TYPE || 'chaveiro',
  attrSteps: process.env.ATTR_STEPS || 's',
  attrStage: process.env.ATTR_STAGE || 'fs',
  attrTimestamp: process.env.ATTR_TIMESTAMP || 'ts'
};

// Cabeçalhos exigidos pelo FIWARE.
// Eles indicam o serviço e o caminho usados na entidade.
function headers() {
  return {
    'Fiware-Service': config.fiwareService,
    'Fiware-ServicePath': config.fiwareServicePath
  };
}

// Normaliza os dados vindos do STH-Comet.
// O objetivo é transformar diferentes formatos possíveis em um padrão único.
function normalizeSthValue(item) {
  return {
    date:
      item.recvTime ||
      item.observedAt ||
      item.date ||
      item.timestamp ||
      new Date().toISOString(),

    value: Number(item.attrValue ?? item.value ?? 0)
  };
}

// Normaliza os dados vindos do QuantumLeap.
// Assim o restante do dashboard pode usar o mesmo formato,
// independentemente do provedor histórico escolhido.
function normalizeQuantumLeapValue(item) {
  return {
    date:
      item.index ||
      item.recvTime ||
      item.date ||
      new Date().toISOString(),

    value: Number(item.value ?? item.attrValue ?? 0)
  };
}

// Busca um atributo histórico no STH-Comet.
// Exemplo: buscar o histórico de passos ou o histórico do estágio da flor.
async function fetchSthAttribute(attrName, lastN = 50) {
  const url =
    `http://${config.vmHost}:${config.historyPort}` +
    `/STH/v1/contextEntities/type/${encodeURIComponent(config.entityType)}` +
    `/id/${encodeURIComponent(config.entityId)}` +
    `/attributes/${encodeURIComponent(attrName)}` +
    `?lastN=${lastN}`;

  const response = await fetch(url, { headers: headers() });

  // Caso a VM ou o STH retorne erro, a mensagem é enviada para o catch da rota.
  if (!response.ok) {
    throw new Error(`STH ${response.status}: ${await response.text()}`);
  }

  const data = await response.json();

  // Caminho padrão da resposta do STH-Comet.
  const values =
    data?.contextResponses?.[0]?.contextElement?.attributes?.[0]?.values || [];

  return values.map(normalizeSthValue);
}

// Busca um atributo histórico no QuantumLeap.
// Essa função é usada caso o provider configurado seja "quantumleap".
async function fetchQuantumLeapAttribute(attrName, lastN = 50) {
  const url =
    `http://${config.vmHost}:${config.historyPort}` +
    `/v2/entities/${encodeURIComponent(config.entityId)}` +
    `/attrs/${encodeURIComponent(attrName)}` +
    `?type=${encodeURIComponent(config.entityType)}` +
    `&lastN=${lastN}`;

  const response = await fetch(url, { headers: headers() });

  if (!response.ok) {
    throw new Error(`QuantumLeap ${response.status}: ${await response.text()}`);
  }

  const data = await response.json();

  // Alguns ambientes retornam "values" diretamente,
  // enquanto outros retornam dentro de attrs.
  const values =
    data?.values ||
    data?.attrs?.[attrName]?.values ||
    [];

  return values.map(normalizeQuantumLeapValue);
}

// Decide qual provedor histórico será usado.
// Isso permite trocar entre STH-Comet e QuantumLeap usando apenas o .env.
async function fetchAttribute(attrName, lastN = 50) {
  if (config.provider === 'quantumleap') {
    return fetchQuantumLeapAttribute(attrName, lastN);
  }

  return fetchSthAttribute(attrName, lastN);
}

// Converte o número do estágio para um nome legível no dashboard.
function stageName(stage) {
  const names = ['Semente', 'Broto', 'Folhas', 'Botao', 'Florida'];

  return names[Number(stage)] || 'Desconhecido';
}

// Gera dados falsos para testar o dashboard sem depender da VM.
// Essa opção é útil para apresentação, desenvolvimento e testes locais.
function gerarHistoricoSimulado() {
  const pontos = [0, 8, 15, 24, 36, 45, 58, 67, 81, 96, 112, 130];

  const agora = Date.now();

  const history = pontos.map((steps, index) => {
    // Cria horários simulados com intervalo de 1 hora entre os registros.
    const date = new Date(
      agora - (pontos.length - 1 - index) * 60 * 60 * 1000
    ).toISOString();

    let stage = 0;

    // Define o estágio da flor de acordo com a quantidade de passos.
    if (steps >= 100) stage = 4;
    else if (steps >= 60) stage = 3;
    else if (steps >= 30) stage = 2;
    else if (steps >= 10) stage = 1;

    return {
      date,
      steps,
      stage,
      stageName: stageName(stage)
    };
  });

  return {
    latest: history[history.length - 1],
    history
  };
}

// Rota usada para consultar a configuração atual do backend.
// Ajuda a verificar se o .env foi carregado corretamente.
app.get('/api/config', (req, res) => {
  res.json({
    provider: config.provider,
    vmHost: config.vmHost,
    historyPort: config.historyPort,
    entityId: config.entityId,
    entityType: config.entityType,

    attrs: {
      steps: config.attrSteps,
      stage: config.attrStage,
      timestamp: config.attrTimestamp
    }
  });
});

// Rota principal consumida pelo dashboard.
// Ela retorna o histórico de passos e o estágio atual da flor.
app.get('/api/history', async (req, res) => {
  try {
    // Quando USE_MOCK_DATA=true, o dashboard usa dados simulados.
    // Isso evita depender da VM durante testes ou apresentação.
    if (process.env.USE_MOCK_DATA === 'true') {
      return res.json({
        ...gerarHistoricoSimulado(),
        source: 'mock'
      });
    }

    // Quantidade de registros históricos solicitados.
    const lastN = Number(req.query.lastN || 50);

    // Busca o histórico de passos.
    const stepsHistory = await fetchAttribute(config.attrSteps, lastN);

    let stageHistory = [];

    // Busca o histórico do estágio da flor.
    // Caso falhe, o dashboard continua usando estágio padrão.
    try {
      stageHistory = await fetchAttribute(config.attrStage, lastN);
    } catch (error) {
      console.warn('Nao foi possivel buscar estagio:', error.message);
    }

    // Combina o histórico de passos com o histórico de estágio.
    const history = stepsHistory.map((stepItem, index) => {
      const stageItem =
        stageHistory[index] ||
        stageHistory[stageHistory.length - 1] ||
        { value: 0 };

      return {
        date: stepItem.date,
        steps: stepItem.value,
        stage: stageItem.value,
        stageName: stageName(stageItem.value)
      };
    });

    // Último registro usado para preencher os cards principais.
    const latest = history[history.length - 1] || {
      date: null,
      steps: 0,
      stage: 0,
      stageName: 'Semente'
    };

    res.json({
      latest,
      history,
      source: 'vm'
    });

  } catch (error) {
    // Caso a conexão com a VM falhe, retorna erro organizado para o frontend.
    res.status(500).json({
      error: 'Nao foi possivel buscar os dados historicos da VM.',
      details: error.message,
      hint: 'Confira VM_HOST, HISTORY_PORT, FIWARE_SERVICE, ENTITY_ID e ENTITY_TYPE no arquivo .env.'
    });
  }
});

// Porta em que o dashboard será executado.
const port = Number(process.env.DASHBOARD_PORT || 3000);

// Inicializa o servidor Express.
app.listen(port, () => {
  console.log(`Dashboard Health Plus rodando em http://localhost:${port}`);

  console.log(
    process.env.USE_MOCK_DATA === 'true'
      ? 'Fonte dos dados: simulacao local'
      : `Fonte dos dados: ${config.provider} em ${config.vmHost}:${config.historyPort}`
  );
});