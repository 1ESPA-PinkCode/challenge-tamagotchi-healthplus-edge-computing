// Meta de passos usada para calcular a porcentagem de progresso.
const GOAL = 100;

// Variável global para guardar o gráfico.
// Isso permite destruir e recriar o gráfico quando novos dados chegarem.
let chart;

// Mapeamento dos estágios da flor.
// Cada número recebido do backend representa uma fase visual da flor.
const stageMap = {
  0: { name: 'Semente', icon: '🌰' },
  1: { name: 'Broto', icon: '🌱' },
  2: { name: 'Folhas', icon: '🌿' },
  3: { name: 'Botão', icon: '🌷' },
  4: { name: 'Florida', icon: '🌸' }
};

// Formata datas para o padrão brasileiro.
// O parâmetro short é usado quando a data precisa aparecer menor,
// como nos cards e no eixo do gráfico.
function formatDate(value, short = false) {
  if (!value) return '--';

  const date = new Date(value);

  // Caso a data venha em um formato inválido,
  // o valor original é exibido para não quebrar a interface.
  if (Number.isNaN(date.getTime())) return value;

  return date.toLocaleString('pt-BR', short ? {
    day: '2-digit',
    month: '2-digit',
    hour: '2-digit',
    minute: '2-digit'
  } : undefined);
}

// Retorna as informações visuais do estágio atual.
// Caso venha um estágio desconhecido, o sistema volta para "Semente".
function getStageInfo(stage) {
  return stageMap[Number(stage)] || stageMap[0];
}

// Normaliza o histórico recebido da API.
// Essa etapa garante que os dados estejam no formato esperado pelo dashboard.
function normalizeHistory(history) {
  return [...(history || [])]

    // Remove itens inválidos ou sem data.
    .filter((item) => item && item.date !== undefined)

    // Converte passos e estágio para número.
    .map((item) => ({
      date: item.date,
      steps: Number(item.steps || 0),
      stage: Number(item.stage || 0)
    }))

    // Ordena do registro mais antigo para o mais recente.
    .sort((a, b) => new Date(a.date) - new Date(b.date));
}

// Atualiza os cards principais do dashboard.
function updateCards(latest) {
  const steps = Number(latest?.steps || 0);
  const stage = Number(latest?.stage || 0);

  // Calcula a porcentagem da meta, limitando o valor máximo a 100%.
  const progress = Math.min(100, Math.round((steps / GOAL) * 100));

  const stageInfo = getStageInfo(stage);

  // Atualiza os textos dos cards.
  document.getElementById('currentSteps').textContent = steps;
  document.getElementById('progressPercent').textContent = `${progress}%`;
  document.getElementById('currentStage').textContent = stageInfo.name;
  document.getElementById('lastUpdate').textContent = formatDate(latest?.date, true);

  // Atualiza a área visual da flor.
  document.getElementById('flowerIcon').textContent = stageInfo.icon;
  document.getElementById('flowerText').textContent = `${stageInfo.name} — ${steps}/${GOAL} passos`;

  // Atualiza a largura da barra de progresso.
  document.getElementById('progressFill').style.width = `${progress}%`;
}

// Atualiza a tabela de histórico.
function updateTable(history) {
  const tbody = document.getElementById('historyTable');

  // Limpa a tabela antes de inserir os dados atualizados.
  tbody.innerHTML = '';

  // Exibe os registros mais recentes primeiro.
  [...history].reverse().forEach((item) => {
    const stageInfo = getStageInfo(item.stage);

    const tr = document.createElement('tr');

    tr.innerHTML = `
      <td>${formatDate(item.date)}</td>
      <td>${item.steps}</td>
      <td>${stageInfo.icon} ${stageInfo.name}</td>
    `;

    tbody.appendChild(tr);
  });
}

// Atualiza o gráfico de passos usando Chart.js.
function updateChart(history) {
  const canvas = document.getElementById('stepsChart');
  const ctx = canvas.getContext('2d');

  // Labels do eixo X com as datas formatadas.
  const labels = history.map((item) => formatDate(item.date, true));

  // Valores do eixo Y com a quantidade de passos.
  const values = history.map((item) => item.steps);

  // Garante que o gráfico tenha pelo menos escala até 100,
  // mesmo quando os valores forem baixos.
  const maxValue = Math.max(100, ...values);

  // Se já existir um gráfico, ele é destruído antes de criar outro.
  // Isso evita sobreposição de gráficos no canvas.
  if (chart) chart.destroy();

  chart = new Chart(ctx, {
    type: 'line',

    data: {
      labels,
      datasets: [{
        label: 'Passos',
        data: values,

        // Suaviza a linha do gráfico.
        tension: 0.3,

        // Mantém o gráfico apenas como linha, sem preenchimento abaixo.
        fill: false,

        // Tamanho dos pontos no gráfico.
        pointRadius: 4,
        pointHoverRadius: 6,

        // Espessura da linha.
        borderWidth: 3
      }]
    },

    options: {
      responsive: true,
      maintainAspectRatio: false,

      // Desativa animações para deixar a atualização mais direta.
      animation: false,

      plugins: {
        legend: {
          labels: {
            color: '#ecfff5'
          }
        },

        tooltip: {
          callbacks: {
            // Texto exibido ao passar o mouse sobre um ponto.
            label: (context) => `Passos: ${context.parsed.y}`
          }
        }
      },

      scales: {
        x: {
          ticks: {
            color: '#9fcab5',

            // Evita que os rótulos fiquem inclinados.
            maxRotation: 0,

            // Pula alguns rótulos automaticamente quando há muitos dados.
            autoSkip: true,
            maxTicksLimit: 8
          },

          grid: {
            color: 'rgba(255,255,255,0.08)'
          }
        },

        y: {
          beginAtZero: true,
          suggestedMax: maxValue,

          ticks: {
            color: '#9fcab5',

            // Garante que os passos apareçam como números inteiros.
            precision: 0
          },

          grid: {
            color: 'rgba(255,255,255,0.08)'
          }
        }
      }
    }
  });
}

// Busca os dados do backend e atualiza todo o dashboard.
async function loadData() {
  const status = document.getElementById('connectionStatus');

  try {
    // Busca os últimos 50 registros de histórico.
    const response = await fetch('/api/history?lastN=50');
    const data = await response.json();

    // Caso a resposta não seja OK, uma mensagem de erro é exibida.
    if (!response.ok) {
      throw new Error(data.details || data.error || 'Erro ao carregar dados');
    }

    // Organiza o histórico antes de usar na interface.
    const history = normalizeHistory(data.history);

    // Usa o último item do histórico como dado mais recente.
    // Caso não exista histórico, usa o latest da API ou valores zerados.
    const latest =
      history[history.length - 1] ||
      data.latest ||
      { steps: 0, stage: 0, date: null };

    // Mostra se os dados estão vindo da simulação ou da VM.
    status.textContent = data.source === 'mock'
      ? 'Simulação ativa'
      : 'VM conectada';

    status.className = 'device-status ok';

    // Atualiza todas as partes visuais do dashboard.
    updateCards(latest);
    updateTable(history);
    updateChart(history);

  } catch (error) {
    console.error(error);

    // Atualiza a interface para indicar erro de conexão.
    status.textContent = 'Erro na conexão';
    status.className = 'device-status error';

    // Exibe a mensagem de erro na área da flor.
    document.getElementById('flowerText').textContent = error.message;
  }
}

// Carrega os dados assim que a página abre.
loadData();

// Atualiza o dashboard automaticamente a cada 10 segundos.
setInterval(loadData, 10000);